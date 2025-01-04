#!/bin/bash

# Script to untrack files containing "//PLEX" in a Git repository
# This script will not untrack itself.

# Step 1: Find all files containing "//PLEX"
# echo "Finding files containing '//PLEX'..."
# grep -rl "//PLEX" . > plex-files.txt

# if [[ ! -s plex-files.txt ]]; then
#   echo "No files containing '//PLEX' found."
#   exit 0
# fi

# Step 2: Untrack the files using `--skip-worktree`, excluding this script
echo "Untracking files..."
while read -r file; do
  # Skip the script file itself
  if [[ "$file" != "./checkout.sh" && -f "$file" ]]; then
    git checkout plex-e613bce65a-11-14-24 -- $file $file
  fi
done < plex-files.txt

# Step 3: Clean up temporary file
rm plex-files.txt

echo "Files containing '//PLEX' are now untracked but remain in the working directory."