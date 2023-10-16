# Generated by devtools/yamaker (pypi).

PY3_LIBRARY()

VERSION(23.9.6)

LICENSE(WTFPL)

PEERDIR(
    contrib/python/PyYAML
)

NO_LINT()

PY_SRCS(
    TOP_LEVEL
    pyaml/__init__.py
    pyaml/__main__.py
    pyaml/cli.py
)

RESOURCE_FILES(
    PREFIX contrib/python/pyaml/py3/
    .dist-info/METADATA
    .dist-info/entry_points.txt
    .dist-info/top_level.txt
)

END()