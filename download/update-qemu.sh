#!/bin/bash

# get script dir:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

echo "Working dir: $SCRIPT_DIR"

# Github repo:
API_URL="https://api.github.com/repos/michaelliao/qemu/releases/latest"

echo "Getting latest release from GitHub API..."

# Get download URL by GitHub API:
ZIP_URL=$(curl -s "$API_URL" | grep "browser_download_url" | grep -o 'https://[^"]*\.zip' | head -n 1)

if [ -z "$ZIP_URL" ]; then
    echo "Error: Unable to fetch the latest release's download link."
    exit 1
fi

ZIP_NAME=$(basename "$ZIP_URL")
echo "Found download url: $ZIP_URL"
echo "Downloading..."

# Download to script dir:
curl -L -o "$ZIP_NAME" "$ZIP_URL"

if [ ! -f "$ZIP_NAME" ]; then
    echo "Error: Download failed!"
    exit 1
fi

# Unzip the downloaded file:
echo "Unzipping $ZIP_NAME..."
unzip -q -o "$ZIP_NAME"

# Get the name of the extracted dir "qemu-riscv-xxx":
NEW_DIR=$(find . -maxdepth 1 -type d -name "qemu-riscv-*" | head -n 1)

if [ -z "$NEW_DIR" ]; then
    echo "Error: Failed to find the extracted qemu-riscv-xxx directory!"
    rm "$ZIP_NAME"
    exit 1
fi

# To convert to relative path:
NEW_DIR_NAME=$(basename "$NEW_DIR")

# Update symbol links:
echo "Cleaning up old symbolic links..."
rm -f "$HOME/.local/bin/qemu-system-riscv32-vga"
rm -f "$HOME/.local/bin/qemu-system-riscv64-vga"

echo "Creating new symbolic links..."
ln -s "$SCRIPT_DIR/$NEW_DIR_NAME/qemu-system-riscv32" "$HOME/.local/bin/qemu-system-riscv32-vga"
ln -s "$SCRIPT_DIR/$NEW_DIR_NAME/qemu-system-riscv64" "$HOME/.local/bin/qemu-system-riscv64-vga"

# Clean up the downloaded zip file:
rm "$ZIP_NAME"

echo "Update complete. Current symbolic links:"
ls -l "$HOME/.local/bin/qemu-system-riscv32-vga" "$HOME/.local/bin/qemu-system-riscv64-vga"
