#!/usr/bin/env bash
set -euo pipefail

# Enter and parse a directory for data
# Check if it exists, if not, create it
# Have a default directory for data, but allow the user to specify one
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA_DIR="$PROJECT_ROOT/data/raw" 

# Set custom path relative to root/data/raw if an argument is provided
if [[ $# -gt 0 ]]; then
    DATA_DIR="$DATA_DIR/$1"
fi

STREAM_NAME="sig00"
CUBE_SIZE=8100

# Create the directory if it doesn't exist
if [ ! -d "$DATA_DIR" ]; then
    echo "Directory $DATA_DIR does not exist. Creating it..."
    mkdir -p "$DATA_DIR"
fi
echo "Saving data to $DATA_DIR..."

echo "Stopping any existing FITS logger for $STREAM_NAME..."
milk-streamFITSlog "$STREAM_NAME" off 2>/dev/null || true
milk-streamFITSlog "$STREAM_NAME" kill 2>/dev/null || true

cleanup() {
    echo
    echo "Stopping FITS logger..."

    # Prevents cleanup from being called multiple times if multiple signals are received
    trap - EXIT INT TERM QUIT

    milk-streamFITSlog "$STREAM_NAME" offc || true
    milk-streamFITSlog "$STREAM_NAME" kill || true

    echo "Data streaming stopped. Data saved in $DATA_DIR."
    
    # Exit cleanly
    exit 0
}

# Run cleanup function on exit, interrupt, or termination signal
# trap cleanup EXIT INT TERM
trap cleanup INT TERM QUIT

# Run the milk-streamFITSlog command to stream data into the specified directory
milk-streamFITSlog -D "$DATA_DIR" -z "$CUBE_SIZE" "$STREAM_NAME" pstart
milk-streamFITSlog "$STREAM_NAME" on

# Wait until Control C is pressed
echo "Streaming data to "$DATA_DIR". Press Ctrl+C to stop."

while true; do
    sleep 1
done
