# Generated by devtools/yamaker (pypi).

PY2_LIBRARY()

VERSION(1.4.2)

LICENSE(MIT)

PEERDIR(
    contrib/python/pytest
)

NO_LINT()

PY_SRCS(
    TOP_LEVEL
    pytest_timeout.py
)

RESOURCE_FILES(
    PREFIX contrib/python/pytest-timeout/py2/
    .dist-info/METADATA
    .dist-info/entry_points.txt
    .dist-info/top_level.txt
)

END()
