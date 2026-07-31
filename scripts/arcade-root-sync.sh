#!/usr/bin/env bash
set -euo pipefail

FAT_DIR="/media/fat"
DRY_RUN=0
DOWNLOAD=1
VERBOSE=0

usage() {
	cat <<'EOF'
Usage: arcade-root-sync.sh [--dry-run] [--fat-dir DIR] [--no-download] [--verbose]

Reconcile root MiSTer arcade and Neo Geo launch entries after update_all.

  --dry-run       Print changes without modifying files
  --fat-dir DIR   MiSTer FAT mount point (default: /media/fat)
  --no-download   Use the cached ArcadeDatabase.csv only
  --verbose       Print extra matching details
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--dry-run) DRY_RUN=1 ;;
		--fat-dir)
			[ "$#" -ge 2 ] || { echo "missing value for --fat-dir" >&2; exit 2; }
			FAT_DIR="$2"
			shift
			;;
		--no-download) DOWNLOAD=0 ;;
		--verbose) VERBOSE=1 ;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "unknown option: $1" >&2
			usage >&2
			exit 2
			;;
	esac
	shift
done

if ! command -v python3 >/dev/null 2>&1; then
	echo "python3 is required" >&2
	exit 1
fi

export ARCADE_ROOT_SYNC_FAT_DIR="$FAT_DIR"
export ARCADE_ROOT_SYNC_DRY_RUN="$DRY_RUN"
export ARCADE_ROOT_SYNC_DOWNLOAD="$DOWNLOAD"
export ARCADE_ROOT_SYNC_VERBOSE="$VERBOSE"

python3 <<'PY'
import csv
import os
import re
import shutil
import sys
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

FAT_DIR = Path(os.environ["ARCADE_ROOT_SYNC_FAT_DIR"])
DRY_RUN = os.environ["ARCADE_ROOT_SYNC_DRY_RUN"] == "1"
DOWNLOAD = os.environ["ARCADE_ROOT_SYNC_DOWNLOAD"] == "1"
VERBOSE = os.environ["ARCADE_ROOT_SYNC_VERBOSE"] == "1"

ARCADE_DIR = FAT_DIR / "_Arcade"
ALT_DIR = ARCADE_DIR / "_alternatives"
NEOGEO_DIR = FAT_DIR / "games" / "NEOGEO"
CONFIG_DIR = FAT_DIR / "Scripts" / ".config" / "arcade-root-sync"
CSV_PATH = CONFIG_DIR / "ArcadeDatabase.csv"
CSV_URL = "https://raw.githubusercontent.com/MiSTer-devel/ArcadeDatabase_MiSTer/main/ArcadeDatabase.csv"
DOWNLOADED_CSV = None


def log(message):
	print(message)


def warn(message):
	print(f"warning: {message}", file=sys.stderr)


def fail(message):
	print(f"error: {message}", file=sys.stderr)
	sys.exit(1)


def change(message):
	log(("DRY-RUN " if DRY_RUN else "") + message)


def ensure_parent(path):
	if not DRY_RUN:
		path.parent.mkdir(parents=True, exist_ok=True)


def download_database():
	global DOWNLOADED_CSV
	if not DOWNLOAD:
		return
	try:
		log(f"refreshing {CSV_PATH}")
		with urllib.request.urlopen(CSV_URL, timeout=20) as response:
			data = response.read()
		if not data.startswith(b"setname,"):
			raise ValueError("download did not look like ArcadeDatabase.csv")
		DOWNLOADED_CSV = data.decode("utf-8-sig", errors="replace")
		if not DRY_RUN:
			CONFIG_DIR.mkdir(parents=True, exist_ok=True)
			tmp = CSV_PATH.with_suffix(".csv.tmp")
			tmp.write_bytes(data)
			tmp.replace(CSV_PATH)
	except Exception as exc:
		warn(f"could not refresh ArcadeDatabase.csv: {exc}")


def read_database():
	if DOWNLOADED_CSV is not None:
		db = {}
		for row in csv.DictReader(DOWNLOADED_CSV.splitlines()):
			setname = (row.get("setname") or "").strip()
			if setname:
				db[setname.lower()] = row
		return db
	if not CSV_PATH.exists():
		warn(f"{CSV_PATH} does not exist; using local MRA metadata only")
		return {}
	db = {}
	try:
		with CSV_PATH.open(newline="", encoding="utf-8-sig") as f:
			for row in csv.DictReader(f):
				setname = (row.get("setname") or "").strip()
				if setname:
					db[setname.lower()] = row
	except Exception as exc:
		warn(f"could not read {CSV_PATH}: {exc}")
	return db


def read_text(path):
	try:
		return path.read_text(encoding="utf-8", errors="replace")
	except Exception as exc:
		warn(f"could not read {path}: {exc}")
		return ""


def parse_xml_text(path, text):
	try:
		text = re.sub(r"&(?!amp;|lt;|gt;|quot;|apos;|#[0-9]+;|#x[0-9a-fA-F]+;)", "&amp;", text)
		return ET.fromstring(text)
	except Exception as exc:
		if VERBOSE:
			warn(f"could not parse XML {path}: {exc}")
		return None


def parse_xml_file(path):
	return parse_xml_text(path, read_text(path))


def child_text(root, name):
	if root is None:
		return ""
	node = root.find(".//" + name)
	return (node.text or "").strip() if node is not None else ""


def tag_text(text, name):
	match = re.search(rf"<{name}\b[^>]*>(.*?)</{name}>", text, flags=re.I | re.S)
	if not match:
		return ""
	value = re.sub(r"<[^>]+>", "", match.group(1))
	return value.strip()


def parse_mra(path):
	text = read_text(path)
	root = parse_xml_text(path, text)
	stem = path.stem
	return {
		"path": path,
		"filename": path.name,
		"stem": stem,
		"name": child_text(root, "name") or tag_text(text, "name") or stem,
		"setname": (child_text(root, "setname") or tag_text(text, "setname")).lower(),
		"players": child_text(root, "players") or tag_text(text, "players"),
		"region": child_text(root, "region") or tag_text(text, "region"),
	}


def max_players(text):
	values = [int(x) for x in re.findall(r"\d+", text or "")]
	return max(values) if values else 0


def player_count(meta, db):
	count = max_players(meta.get("players", ""))
	row = db.get(meta.get("setname", ""))
	if row:
		count = max(count, max_players(row.get("players", "")))
	return count


def normalized_title(text):
	text = re.sub(r"\[[^\]]*\]", " ", text)
	text = re.sub(r"\([^)]*\)", " ", text)
	text = re.sub(r"\b(ver|version|set|rev|revision)\b.*$", " ", text, flags=re.I)
	text = text.replace("&amp;", "&")
	text = re.sub(r"[^a-z0-9]+", " ", text.lower())
	return re.sub(r"\s+", " ", text).strip()


def is_two_player_variant(meta, db):
	fields = [meta.get("filename", ""), meta.get("name", ""), meta.get("stem", "")]
	row = db.get(meta.get("setname", ""))
	if row:
		fields.extend([row.get("name", ""), row.get("version", ""), row.get("parent_title", "")])
	text = " ".join(fields).lower()
	return bool(re.search(r"\b2\s*(players?|p)\b", text))


def title_key(meta, db):
	row = db.get(meta.get("setname", ""))
	if row:
		parent = (row.get("parent_title") or "").strip()
		if parent:
			return normalized_title(parent)
	return normalized_title(meta.get("name") or meta.get("stem") or "")


def normalized_region(value):
	value = (value or "").strip().lower()
	if value in {"usa", "u.s.a.", "u.s.", "america"}:
		return "us"
	if value in {"europe", "eu", "eur"}:
		return "europe"
	if value in {"japan", "jp"}:
		return "japan"
	if value in {"uk", "united kingdom", "gb", "great britain"}:
		return "uk"
	if value in {"world", "w"}:
		return "world"
	return value


def region_score(top, candidate, db):
	top_region = normalized_region(top.get("region"))
	cand_region = normalized_region(candidate.get("region"))
	top_row = db.get(top.get("setname", ""))
	cand_row = db.get(candidate.get("setname", ""))
	if top_row and not top_region:
		top_region = normalized_region(top_row.get("region"))
	if cand_row and not cand_region:
		cand_region = normalized_region(cand_row.get("region"))
	if top_region and cand_region and top_region == cand_region:
		return 0
	if top_region == "us" and cand_region == "world":
		return 1
	if top_region == "world" and cand_region in {"us", "usa", "world"}:
		return 1
	if top_region == "europe" and cand_region in {"uk", "world"}:
		return 1
	return 2


def choose_two_player(top, alternatives, db):
	key = title_key(top, db)
	if not key:
		return None
	candidates = []
	for alt in alternatives:
		if title_key(alt, db) != key:
			continue
		if not is_two_player_variant(alt, db):
			continue
		candidates.append(alt)
	if not candidates:
		return None
	candidates.sort(key=lambda m: (
		region_score(top, m, db),
		len(m["filename"]),
		m["filename"].lower(),
	))
	return candidates[0]


def xml_escape(value):
	return (
		value.replace("&", "&amp;")
		.replace('"', "&quot;")
		.replace("<", "&lt;")
		.replace(">", "&gt;")
	)


def sanitize_filename(name):
	name = re.sub(r'[/:*?"<>|\\]', " - ", name)
	name = re.sub(r"\s+", " ", name).strip().strip(".")
	return name or "Unknown"


def neogeo_titles():
	romsets = NEOGEO_DIR / "romsets.xml"
	titles = {}
	root = parse_xml_file(romsets) if romsets.exists() else None
	if root is None:
		return titles
	for romset in root.findall(".//romset"):
		altname = (romset.attrib.get("altname") or "").strip()
		names = [x.strip().lower() for x in (romset.attrib.get("name") or "").split(",") if x.strip()]
		for name in names:
			if altname:
				titles[name] = altname
	return titles


def build_neogeo_mgl(neo_file, title):
	rom_name = xml_escape(neo_file.name)
	return (
		"<mistergamedescription>\n"
		"\t<rbf>_Console/NeoGeo</rbf>\n"
		f"\t<file delay=\"1\" type=\"f\" index=\"1\" path=\"{rom_name}\"/>\n"
		"</mistergamedescription>\n"
	)


def root_entries(suffix):
	return sorted([p for p in FAT_DIR.iterdir() if p.is_file() or p.is_symlink() if p.suffix.lower() == suffix])


def replace_symlink(path, target):
	current = None
	if path.is_symlink():
		current = os.readlink(path)
		if current == str(target):
			return False
	if path.exists() or path.is_symlink():
		change(f"replace {path}")
		if not DRY_RUN:
			path.unlink()
	else:
		change(f"create {path}")
	if not DRY_RUN:
		os.symlink(str(target), str(path))
	return True


def write_file_if_changed(path, content):
	if path.exists() and not path.is_symlink():
		try:
			if path.read_text(encoding="utf-8", errors="replace") == content:
				return False
		except Exception:
			pass
	if path.exists() or path.is_symlink():
		change(f"replace {path}")
	else:
		change(f"create {path}")
	if not DRY_RUN:
		fd, tmp_name = tempfile.mkstemp(prefix=path.name + ".", dir=str(path.parent))
		with os.fdopen(fd, "w", encoding="utf-8") as f:
			f.write(content)
		os.chmod(tmp_name, 0o644)
		os.replace(tmp_name, path)
	return True


def remove_path(path):
	change(f"remove {path}")
	if not DRY_RUN:
		if path.is_dir() and not path.is_symlink():
			shutil.rmtree(path)
		else:
			path.unlink()


def ensure_cores_link():
	cores = FAT_DIR / "cores"
	target = ARCADE_DIR / "cores"
	if not target.exists():
		warn(f"{target} does not exist; root MRA symlinks may not launch")
		return
	if cores.is_symlink() and os.readlink(cores) == str(target):
		return
	if cores.exists() or cores.is_symlink():
		warn(f"{cores} already exists and was left unchanged")
		return
	change(f"create {cores} -> {target}")
	if not DRY_RUN:
		os.symlink(str(target), str(cores))


def main():
	if not FAT_DIR.exists():
		fail(f"{FAT_DIR} does not exist")
	if not ARCADE_DIR.exists():
		fail(f"{ARCADE_DIR} does not exist")

	download_database()
	db = read_database()

	top_mras = [parse_mra(p) for p in sorted(ARCADE_DIR.glob("*.mra"), key=lambda p: p.name.lower())]
	alt_mras = [parse_mra(p) for p in sorted(ALT_DIR.rglob("*.mra"), key=lambda p: str(p).lower())] if ALT_DIR.exists() else []

	desired_mra = {}
	replacements = 0
	for top in top_mras:
		target = top["path"]
		if player_count(top, db) >= 3:
			alt = choose_two_player(top, alt_mras, db)
			if alt:
				target = alt["path"]
				replacements += 1
				if VERBOSE:
					log(f"2-player replacement: {top['filename']} -> {alt['filename']}")
		desired_mra[target.name] = target

	titles = neogeo_titles()
	desired_mgl = {}
	if NEOGEO_DIR.exists():
		for neo in sorted(NEOGEO_DIR.glob("*.neo"), key=lambda p: p.name.lower()):
			title = titles.get(neo.stem.lower(), neo.stem)
			root_name = sanitize_filename(title) + ".mgl"
			desired_mgl[root_name] = build_neogeo_mgl(neo, title)
	else:
		warn(f"{NEOGEO_DIR} does not exist; skipping Neo Geo MGL generation")

	for path in root_entries(".mra"):
		if path.name not in desired_mra:
			remove_path(path)
	for path in root_entries(".mgl"):
		if path.name not in desired_mgl:
			remove_path(path)

	for name, target in desired_mra.items():
		replace_symlink(FAT_DIR / name, target)

	for name, content in desired_mgl.items():
		write_file_if_changed(FAT_DIR / name, content)

	ensure_cores_link()

	log(f"arcade-root-sync: desired {len(desired_mra)} arcade MRA entries, {replacements} 2-player replacements, {len(desired_mgl)} Neo Geo MGL entries")
	if DRY_RUN:
		log("arcade-root-sync: dry run complete; no files changed")
	else:
		log("arcade-root-sync: reconciliation complete")


if __name__ == "__main__":
	main()
PY
