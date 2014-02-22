from common import update_from_env

PROXY = None

TOKEN = None
TOKEN_PATH = None
USE_TOKEN = True

ACCEPT_ENCODING = "gzip, identity"
CONTENT_ENCODING = "gzip"

REQUEST_RETRY_TIMEOUT = 20 * 1000
REQUEST_TIMEOUT = 2 * 60 * 1000
REQUEST_RETRY_COUNT = int(REQUEST_TIMEOUT / REQUEST_RETRY_TIMEOUT)
CONTENT_CHUNK_SIZE = 10 * 1024

# COMPAT(ignat): remove option when version 14 become stable
RETRY_VOLATILE_COMMANDS = False

FORCE_IPV4 = False
FORCE_IPV6 = False

update_from_env(globals())
