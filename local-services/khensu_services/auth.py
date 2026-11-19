import hmac
import logging
from pathlib import Path
from fastapi import Security
from khensu_services.common.errors import internal_error, unauthorized
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials

from khensu_services.common.security_log import KIND_AUTH_FAILURE, record_event

_log = logging.getLogger(__name__)
security = HTTPBearer(auto_error=False)

def get_token_path() -> Path:
    from khensu_services.common.config import config_dir
    return config_dir() / "token.txt"


def read_local_token() -> str | None:
    """The per-machine token as stored in token.txt (stripped), or None when
    the file does not exist. Raises OSError/UnicodeError on a read failure."""
    token_path = get_token_path()
    if not token_path.exists():
        return None
    with open(token_path, "r", encoding="utf-8") as f:
        return f.read().strip()


def check_bearer_token(token: str | None) -> None:
    """Raise the 401/500 HTTPException for a missing/invalid bearer token.

    Shared by the ``verify_token`` dependency and the server's pre-body auth
    middleware so both enforce exactly the same rule."""
    if token is None:
        record_event(KIND_AUTH_FAILURE, "missing Authorization header")
        unauthorized("Authorization header missing")
    try:
        local_token = read_local_token()
    except Exception as e:
        # SVC-09: this is reachable WITHOUT authentication, so the reply must
        # not carry the OS error (it named the config path / user profile).
        # The cause goes to the service log only.
        _log.error("Failed to read the local token file: %s", e)
        internal_error("Service authentication is unavailable; see the service log.", cause=e)
    if local_token is None:
        unauthorized("Local token file not found. Is Khensu running?")
    # Compare bytes, not str (AUDIT-076): compare_digest(str, str) raises
    # TypeError on any non-ASCII character, which surfaced as a 500 instead of
    # being counted as an auth failure.
    if not hmac.compare_digest(token.encode("utf-8", "surrogatepass"),
                               local_token.encode("utf-8", "surrogatepass")):
        # Never log `token` itself — only that a mismatch occurred (audit SEC-07).
        record_event(KIND_AUTH_FAILURE, "bearer token mismatch")
        unauthorized("Invalid session token")


def bearer_from_header(authorization: str | None) -> str | None:
    """The credentials of an ``Authorization: Bearer <token>`` header, or None
    (same parsing as ``HTTPBearer``: scheme case-insensitive, token required)."""
    if not authorization:
        return None
    scheme, _, credentials = authorization.partition(" ")
    credentials = credentials.strip()
    if scheme.lower() != "bearer" or not credentials:
        return None
    return credentials


def verify_token(credentials: HTTPAuthorizationCredentials | None = Security(security)):
    check_bearer_token(credentials.credentials if credentials is not None else None)
