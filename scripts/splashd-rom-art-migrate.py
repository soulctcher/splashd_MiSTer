#!/usr/bin/env python3
"""Apply a validated title-to-ROM wallpaper mapping without deleting source art."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path


DEFAULT_PLAN = Path("/media/fat/Scripts/.config/splashd-rom-art-migration/migration_plan.json")
CONFIG_DIR = Path("/media/fat/Scripts/.config/splashd-rom-art-migration")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class Reporter:
    def __init__(self, stamp: str, apply: bool):
        log_dir = CONFIG_DIR / "logs"
        log_dir.mkdir(parents=True, exist_ok=True)
        mode = "apply" if apply else "dry-run"
        self.text_path = log_dir / f"{stamp}-{mode}.log"
        self.jsonl_path = log_dir / f"{stamp}-{mode}.jsonl"
        self.text = self.text_path.open("w", encoding="utf-8")
        self.jsonl = self.jsonl_path.open("w", encoding="utf-8")

    def emit(self, action: str, result: str, **fields) -> None:
        record = {"action": action, "result": result, **{key: str(value) for key, value in fields.items() if value is not None}}
        self.jsonl.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")
        details = " ".join(f"{key}={value!r}" for key, value in record.items() if key not in {"action", "result"})
        self.text.write(f"{action}: {result}" + (f" {details}" if details else "") + "\n")
        self.text.flush()
        self.jsonl.flush()

    def close(self) -> None:
        self.text.close()
        self.jsonl.close()


def casefold_inventory(directory: Path) -> dict[str, Path]:
    inventory = {}
    for path in directory.iterdir():
        if path.name.startswith(".") or path.suffix.lower() not in {".png", ".jpg", ".jpeg"}:
            continue
        key = path.name.casefold()
        if key in inventory:
            raise RuntimeError(f"case-insensitive filename collision: {inventory[key].name!r} and {path.name!r}")
        inventory[key] = path
    return inventory


def validate_plan(plan: dict, inventory: dict[str, Path]) -> None:
    source_names = [entry["source"] for entry in plan["plans"]]
    if len(source_names) != len({name.casefold() for name in source_names}):
        raise RuntimeError("migration plan contains duplicate source filenames")
    missing = [name for name in source_names if name.casefold() not in inventory]
    if missing:
        raise RuntimeError(f"{len(missing)} planned source file(s) are missing; first: {missing[0]}")

    final_names = []
    for entry in plan["plans"]:
        if entry["action"] != "migrate":
            continue
        final_names.append(entry["master"])
        final_names.extend(entry.get("aliases") or [])
    if len(final_names) != len({name.casefold() for name in final_names}):
        raise RuntimeError("migration plan contains case-insensitive destination collisions")


def run(plan_path: Path, apply: bool) -> int:
    plan = json.loads(plan_path.read_text(encoding="utf-8"))
    wallpaper_dir = Path(plan["wallpaper_dir"])
    archive_root = Path(plan["archive_root"])
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    reporter = Reporter(stamp, apply)
    try:
        inventory = casefold_inventory(wallpaper_dir)
        validate_plan(plan, inventory)
        reporter.emit(
            "preflight",
            "passed",
            source_count=len(plan["plans"]),
            migrate_count=sum(entry["action"] == "migrate" for entry in plan["plans"]),
            archive_count=sum(entry["action"] == "archive" for entry in plan["plans"]),
            alias_count=sum(len(entry.get("aliases") or []) for entry in plan["plans"]),
        )
        if not apply:
            for entry in plan["plans"]:
                reporter.emit(
                    entry["action"],
                    "planned",
                    source=entry["source"],
                    target=entry.get("master"),
                    alias_count=len(entry.get("aliases") or []),
                    reason=entry.get("reason"),
                )
            return 0

        stage_dir = CONFIG_DIR / "staging" / stamp
        archive_dir = archive_root / f"wallpapers_rom_migration_{stamp}"
        stage_dir.mkdir(parents=True, exist_ok=False)
        archive_dir.mkdir(parents=True, exist_ok=False)
        staged = {}
        for index, entry in enumerate(plan["plans"]):
            source = inventory[entry["source"].casefold()]
            staged_path = stage_dir / f"{index:04d}{source.suffix.lower()}"
            source_hash = sha256(source)
            os.replace(source, staged_path)
            staged[entry["source"].casefold()] = (staged_path, source_hash)
            reporter.emit("stage", "moved", source=source, target=staged_path, sha256=source_hash)

        manifest_files = {}
        migrated = []
        for entry in plan["plans"]:
            staged_path, source_hash = staged[entry["source"].casefold()]
            if entry["action"] == "archive":
                target = archive_dir / entry["source"]
                os.replace(staged_path, target)
                reporter.emit("archive", "moved", source=entry["source"], target=target, sha256=source_hash, reason=entry.get("reason"))
                continue

            master = wallpaper_dir / entry["master"]
            os.replace(staged_path, master)
            manifest_files[master.name] = {
                "kind": "regular",
                "source_filename": entry["source"],
                "sha256": source_hash,
            }
            migrated.append((entry, master, source_hash))
            reporter.emit("master", "renamed", source=entry["source"], target=master, sha256=source_hash)

        for entry, master, source_hash in migrated:
            for alias_name in entry.get("aliases") or []:
                alias = wallpaper_dir / alias_name
                if alias.name.casefold() == master.name.casefold():
                    continue
                alias.symlink_to(master.name)
                manifest_files[alias.name] = {
                    "kind": "symlink",
                    "link_target": master.name,
                    "sha256": source_hash,
                }
                reporter.emit("alias", "linked", source=master, target=alias, link_target=master.name, sha256=source_hash)

        broken = []
        for name, metadata in manifest_files.items():
            path = wallpaper_dir / name
            if not path.exists():
                broken.append(name)
            elif metadata["kind"] == "regular" and sha256(path) != metadata["sha256"]:
                broken.append(name)
        if broken:
            raise RuntimeError(f"verification failed for {len(broken)} file(s); first: {broken[0]}")

        manifest = {
            "version": 1,
            "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "wallpaper_dir": str(wallpaper_dir),
            "archive_batch": str(archive_dir),
            "summary": {
                "regular_masters": sum(item["kind"] == "regular" for item in manifest_files.values()),
                "symlink_aliases": sum(item["kind"] == "symlink" for item in manifest_files.values()),
                "archived_sources": sum(entry["action"] == "archive" for entry in plan["plans"]),
                "broken": len(broken),
            },
            "files": manifest_files,
        }
        manifest_path = CONFIG_DIR / "current_mapping.json"
        manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False, sort_keys=True) + "\n", encoding="utf-8")
        try:
            stage_dir.rmdir()
        except OSError:
            pass
        reporter.emit("manifest", "written", target=manifest_path, file_count=len(manifest_files))
        return 0
    finally:
        reporter.close()
        print(f"Text log: {reporter.text_path}")
        print(f"JSONL log: {reporter.jsonl_path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", type=Path, default=DEFAULT_PLAN)
    parser.add_argument("--apply", action="store_true", help="apply the migration; omitted means dry-run")
    args = parser.parse_args()
    return run(args.plan, args.apply)


if __name__ == "__main__":
    raise SystemExit(main())
