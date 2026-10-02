#!/usr/bin/env bash
# Put this project on GitHub as a public repo and publish its website (GitHub Pages).
#   First time:  bash publish.sh [repo-name]
#   Updates:     git add -A && git commit -m "what changed" && bash publish.sh
# Needs git and the GitHub CLI "gh", logged in (gh auth login).
#
# The code lives on the "main" branch. The website is the web/ folder, pushed to a "gh-pages"
# branch that GitHub Pages serves.
set -euo pipefail
cd "$(dirname "$0")"
NAME="${1:-web-pocketstation}"
DESC="PocketStation emulator for phones and browsers: plain JavaScript, installable, bring your own BIOS and memory cards."
command -v git >/dev/null || { echo "git is not installed"; exit 1; }
command -v gh >/dev/null || { echo "The GitHub CLI (gh) is not installed. Install it (https://cli.github.com), run 'gh auth login', then run this again. Or follow 'Publishing by hand' in README.md."; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "Run 'gh auth login' first."; exit 1; }

[ -d .git ] || git init -q -b main
if ! git rev-parse HEAD >/dev/null 2>&1; then
  git add -A
  FIRST=1
fi
# belt and braces on top of .gitignore: refuse to publish BIOS / game / save / personal files
BAD="$(git ls-files | grep -Ei '\.(min|eep|srm|zip|bin|mcd|mcr|gme)$|(^|/)config\.json$' || true)"
if [ -n "$BAD" ]; then echo "Refusing to publish these files (BIOS, games, saves or personal settings):"; echo "$BAD"; exit 1; fi
if [ -n "${FIRST:-}" ]; then
  git commit -q -m "Initial public release" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_015HcAktbA59g7nGYTyi5b9s"
fi
git branch -M main
if [ -n "$(git status --porcelain)" ]; then echo "Note: there are uncommitted changes; only committed work is published."; fi

USER_LOGIN="$(gh api user -q .login)"
if ! git remote get-url origin >/dev/null 2>&1; then
  gh repo create "$NAME" --public --source=. --remote=origin --description "$DESC"
fi
git push -u origin main
# the website: web/ becomes the root of the gh-pages branch
git push origin "$(git subtree split --prefix web main)":refs/heads/gh-pages --force
gh api -X POST "repos/$USER_LOGIN/$NAME/pages" -f "source[branch]=gh-pages" -f "source[path]=/" >/dev/null 2>&1 \
  || gh api -X PUT "repos/$USER_LOGIN/$NAME/pages" -f "source[branch]=gh-pages" -f "source[path]=/" >/dev/null 2>&1 \
  || echo "Could not switch on Pages automatically: repo Settings → Pages → Deploy from a branch → gh-pages, / (root)."
gh repo edit "$USER_LOGIN/$NAME" --homepage "https://$USER_LOGIN.github.io/$NAME/" >/dev/null 2>&1 || true
echo
echo "Code:  https://github.com/$USER_LOGIN/$NAME"
echo "Site:  https://$USER_LOGIN.github.io/$NAME/   (live a minute or two after GitHub finishes building it)"
