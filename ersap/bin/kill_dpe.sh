#!/bin/bash

# Find PIDs matching c_dpe or j_dpe (excluding the grep itself)
PIDS=$(ps -ef | grep -E 'c_dpe|j_dpe' | grep -v grep | awk '{print $2}')

if [ -z "$PIDS" ]; then
    echo "No matching processes found."
    exit 0
fi

echo "Found processes:"
ps -fp $PIDS

echo ""
echo "Killing processes..."

for PID in $PIDS; do
    echo "Killing PID $PID"
    kill -9 $PID
done

echo "Done."
