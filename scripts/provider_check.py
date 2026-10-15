#!/usr/bin/env python3
import sys
from providers import check

try:
    print(check(*sys.argv[1:4]))
except Exception as error:
    # Never expose provider response bodies, credential values, or subprocess stderr.
    print(str(error) if isinstance(error, RuntimeError) else "Provider check failed or timed out")
    sys.exit(1)
