# Environment for building / deploying the Guild II ultrawide fix.
# Copy or `source` this file (edit the paths first):
#
#     source scripts/env.example.sh
#
# Only GUILD2_LIVE_DIR is required for `make deploy`.

# Live game directory -- where you actually run the game from (contains GuildII.exe).
# This is where the built d3d9.dll + uw_fix.ini get deployed.
export GUILD2_LIVE_DIR="$HOME/.local/share/Steam/steamapps/common/The Guild 2 Renaissance"

# Pristine, untouched game files -- a reference copy made before any modding.
# Used only for diffing / reverse-engineering; not needed to build or deploy.
export GUILD2_ORIG_DIR="$HOME/guild2-original"
