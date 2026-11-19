"""Khensu local analysis services."""
import os as _os

# numpy / scipy's OpenBLAS starts a thread per core for every matrix product.
# The service already runs up to 8 analyses at once on its DSP pool, so each
# of those products then fights the others for the same cores: in the
# 2026-11-18 stress test six concurrent mel projections took 18 s instead of
# 1.5 s. Worse, OpenBLAS's own thread pool is entered through a lock shared
# by every calling thread, and under that load the service crashed inside it
# (access violation in libscipy_openblas64, RtlEnterCriticalSection). With
# one thread OpenBLAS never uses its pool or that lock, and the heavy routes
# ran fastest that way on a single request too. Set before anything imports
# numpy; an explicit OPENBLAS_NUM_THREADS still wins. torch is capped
# separately (common/torch_runtime.py) and does not read this variable.
_os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")

__version__ = "1.0.0"
