# Generated by devtools/yamaker from nixpkgs 22.05.

LIBRARY()

LICENSE(
    Apache-2.0 AND
    MIT
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(3.7.2)

ORIGINAL_SOURCE(https://github.com/uber/h3/archive/v3.7.2.tar.gz)

ADDINCL(
    contrib/libs/h3/h3lib/include
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

CFLAGS(
    -DH3_PREFIX=
)

SRCS(
    h3lib/lib/algos.c
    h3lib/lib/baseCells.c
    h3lib/lib/bbox.c
    h3lib/lib/coordijk.c
    h3lib/lib/faceijk.c
    h3lib/lib/geoCoord.c
    h3lib/lib/h3Index.c
    h3lib/lib/h3UniEdge.c
    h3lib/lib/linkedGeo.c
    h3lib/lib/localij.c
    h3lib/lib/mathExtensions.c
    h3lib/lib/polygon.c
    h3lib/lib/vec2d.c
    h3lib/lib/vec3d.c
    h3lib/lib/vertex.c
    h3lib/lib/vertexGraph.c
)

END()