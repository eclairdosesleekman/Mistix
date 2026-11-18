#!/usr/bin/env python3
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[1]
threads = (root / 'src/kernel/mistix_threads.inc').read_text()
types = (root / 'src/kernel/mistix_types.inc').read_text()
required = ['THREAD_PRIORITY_LEVELS = 32', 'THREAD_IDLE_INDEX = THREAD_MAX_COUNT - 1', 'function ThreadCreatePriority', 'function ThreadSetPriority', 'procedure ThreadIdleHalt', 'procedure ThreadBlockCurrent', 'function ThreadWakeWaiters', 'procedure ThreadSchedule']
missing = [item for item in required if item not in threads]
if 'THREAD_MAX_COUNT = 13;' not in types: missing.append('dedicated idle slot')
start = threads.index('procedure ThreadSchedule;')
end = threads.index('procedure ThreadYield;', start)
if 'HaltForever' in threads[start:end]: missing.append('scheduler must not halt when workers block')
if missing:
    print('Phase 20 contract check failed:')
    for item in missing: print('  missing:', item)
    sys.exit(1)
print('Phase 20 scheduler foundation source contract passed.')
