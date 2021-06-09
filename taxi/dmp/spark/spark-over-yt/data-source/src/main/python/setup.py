from distutils.util import convert_path

import setuptools

ver_path = convert_path('spyt/version.py')
with open(ver_path) as ver_file:
    exec(ver_file.read())

setuptools.setup(
    name="yandex-spyt",
    version=__version__,
    author="Alexandra Belousova",
    author_email="sashbel@yandex-team.ru",
    description="Spark over YT high-level client",
    url="https://github.yandex-team.ru/taxi-dwh/spark-over-yt",
    packages=setuptools.find_packages(),
    install_requires=[
        "yandex-pyspark==3.0.1+1.6.0",
        "yandex-yt>=0.9.29",
        "pyarrow==0.15.1",
        "pyyaml"
    ],
)
