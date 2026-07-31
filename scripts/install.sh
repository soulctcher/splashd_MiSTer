#!/bin/sh
set -eu

FAT_DIR="${SPLASHD_FAT_DIR:-/media/fat}"
SPLASHD_DIR="$FAT_DIR/splashd"
WALLPAPER_DIR="$SPLASHD_DIR/wallpapers"
INI_FILE="$FAT_DIR/MiSTer.ini"
LINUX_DIR="$FAT_DIR/linux"
STARTUP_FILE="$LINUX_DIR/user-startup.sh"
SPLASHD_BIN="$SPLASHD_DIR/splashd"
BEGIN_MARK="# BEGIN splashd managed block"
END_MARK="# END splashd managed block"

timestamp() {
	date -u +"%Y%m%dT%H%M%SZ"
}

info() {
	printf '%s\n' "$*"
}

warn() {
	printf 'WARNING: %s\n' "$*" >&2
}

die() {
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

require_dir() {
	[ -d "$1" ] || die "$1 does not exist"
}

require_file() {
	[ -f "$1" ] || die "$1 does not exist"
}

backup_file() {
	src="$1"
	cp "$src" "$src.bak.$(timestamp)"
}

ensure_log_file_entry() {
	require_file "$INI_FILE"

	tmp="$INI_FILE.tmp.$$"
	changed=0
	found=0

	awk '
		BEGIN { changed = 0; found = 0 }
		{
			if ($0 ~ /^[[:space:]]*;?[[:space:]]*log_file_entry[[:space:]]*=/) {
				found = 1
				if ($0 != "log_file_entry=1") changed = 1
				print "log_file_entry=1"
				next
			}
			print
		}
		END {
			if (!found) {
				print "log_file_entry=1"
				changed = 1
			}
			exit changed ? 2 : 0
		}
	' "$INI_FILE" > "$tmp" || rc=$?

	rc="${rc:-0}"
	if [ "$rc" = 2 ]; then
		backup_file "$INI_FILE"
		mv "$tmp" "$INI_FILE"
		info "Enabled log_file_entry=1 in $INI_FILE"
	elif [ "$rc" = 0 ]; then
		rm -f "$tmp"
		info "log_file_entry=1 already enabled"
	else
		rm -f "$tmp"
		die "failed to update $INI_FILE"
	fi
}

managed_block_file() {
	out="$1"
	{
		printf '%s\n' "$BEGIN_MARK"
		printf '%s\n' 'if [ -x /media/fat/splashd/splashd ] && ! pidof splashd >/dev/null 2>&1; then'
		printf '\t%s\n' '/media/fat/splashd/splashd'
		printf '%s\n' 'fi'
		printf '%s\n' "$END_MARK"
	} > "$out"
}

ensure_startup() {
	require_dir "$LINUX_DIR"

	if [ ! -e "$STARTUP_FILE" ]; then
		{
			printf '%s\n' '#!/bin/sh'
			printf '\n'
		} > "$STARTUP_FILE"
		chmod +x "$STARTUP_FILE"
		info "Created $STARTUP_FILE"
	fi

	require_file "$STARTUP_FILE"
	backup_file "$STARTUP_FILE"

	clean="$STARTUP_FILE.clean.$$"
	block="$STARTUP_FILE.block.$$"
	tmp="$STARTUP_FILE.tmp.$$"

	awk -v begin="$BEGIN_MARK" -v end="$END_MARK" '
		$0 == begin { skip = 1; next }
		$0 == end { skip = 0; next }
		!skip { print }
	' "$STARTUP_FILE" > "$clean"

	managed_block_file "$block"

	awk -v block="$block" '
		{
			lines[++n] = $0
			if ($0 ~ /\/media\/fat\/pixelcade/ || $0 ~ /runpixelcade\.sh/ || $0 ~ /pixelcadeLink\.sh/) {
				last_pixelcade = n
			}
		}
		END {
			for (i = 1; i <= n; i++) {
				print lines[i]
				if (i == last_pixelcade) {
					print ""
					while ((getline b < block) > 0) print b
					inserted = 1
				}
			}
			if (!inserted) {
				if (n && lines[n] != "") print ""
				while ((getline b < block) > 0) print b
			}
		}
	' "$clean" > "$tmp"

	mv "$tmp" "$STARTUP_FILE"
	chmod +x "$STARTUP_FILE"
	rm -f "$clean" "$block"
	info "Updated $STARTUP_FILE"
}

restart_splashd() {
	[ -x "$SPLASHD_BIN" ] || die "$SPLASHD_BIN is missing or not executable"

	if [ "${SPLASHD_NO_RESTART:-0}" = "1" ]; then
		info "Skipping splashd restart because SPLASHD_NO_RESTART=1"
		return
	fi

	if pidof splashd >/dev/null 2>&1; then
		kill $(pidof splashd) || true
		sleep 1
	fi

	"$SPLASHD_BIN"
	sleep 1

	pidof splashd >/dev/null 2>&1 || die "splashd did not start"
	info "splashd is running: $(pidof splashd)"
}

warn_default_menu_image() {
	if [ ! -f "$FAT_DIR/menu.png" ] && [ ! -f "$FAT_DIR/menu.jpg" ] && [ ! -f "$FAT_DIR/menu.jpeg" ]; then
		warn "No default menu image found at $FAT_DIR/menu.png, $FAT_DIR/menu.jpg, or $FAT_DIR/menu.jpeg"
	fi
}

main() {
	require_dir "$FAT_DIR"
	require_dir "$SPLASHD_DIR"
	require_dir "$LINUX_DIR"
	require_file "$INI_FILE"

	mkdir -p "$WALLPAPER_DIR"
	info "Ensured $WALLPAPER_DIR exists"

	ensure_log_file_entry
	ensure_startup
	warn_default_menu_image
	restart_splashd

	cat <<EOF

splashd installation complete.

Recommended MiSTer setup:
- Keep a default /media/fat/menu.jpg, /media/fat/menu.png, or /media/fat/menu.jpeg.
- Use F1 in MiSTer until the menu is set to display the custom menu image/wallpaper mode.
- Set osd_timeout to 180 or higher to reduce idle repaint transitions while browsing.
EOF
}

main "$@"
