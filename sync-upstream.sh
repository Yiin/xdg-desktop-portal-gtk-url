#!/usr/bin/env bash
set -euo pipefail

cd ~/Projects/xdg-desktop-portal-gtk-url

git fetch upstream main 2>/dev/null

# Nothing to do if already up to date
if git merge-base --is-ancestor upstream/main HEAD; then
  exit 0
fi

echo "New upstream commits found, rebasing..."

# Try clean rebase first
if git rebase upstream/main 2>/dev/null; then
  echo "Clean rebase, rebuilding and installing..."
  ninja -C build
  systemctl --user stop xdg-desktop-portal-gtk.service
  sudo cp build/src/xdg-desktop-portal-gtk /usr/lib/xdg-desktop-portal-gtk
  systemctl --user start xdg-desktop-portal-gtk.service
  git push --force-with-lease origin main
  notify-send "xdg-desktop-portal-gtk" "Synced with upstream and installed"
  exit 0
fi

# Conflicts — let Claude resolve
echo "Conflicts detected, running Claude..."
git rebase --abort

# Start rebase again for Claude to work with
git rebase upstream/main || true

if claude --dangerously-skip-permissions -p \
  "You are rebasing a fork of xdg-desktop-portal-gtk. There are merge conflicts in src/filechooser.c. Our patch adds URL download support and a filename bar to the file chooser dialog. Resolve the conflicts keeping both upstream changes and our additions. After resolving, stage fixed files and run: git rebase --continue"; then
  echo "Claude resolved conflicts, rebuilding..."
  ninja -C build
  systemctl --user stop xdg-desktop-portal-gtk.service
  sudo cp build/src/xdg-desktop-portal-gtk /usr/lib/xdg-desktop-portal-gtk
  systemctl --user start xdg-desktop-portal-gtk.service
  git push --force-with-lease origin main
  notify-send "xdg-desktop-portal-gtk" "Synced with upstream (conflicts resolved by Claude)"
else
  git rebase --abort 2>/dev/null || true
  notify-send -u critical "xdg-desktop-portal-gtk" "Upstream sync failed — manual resolution needed"
  exit 1
fi
