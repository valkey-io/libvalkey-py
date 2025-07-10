#!/usr/bin/env python3

try:
    from setuptools import Extension, setup
except ImportError:
    from distutils.core import setup, Extension

import glob
import importlib
import io
import sys


def version():
    loader = importlib.machinery.SourceFileLoader(
        "libvalkey.version", "libvalkey/version.py"
    )
    module = loader.load_module()
    return module.__version__


def get_sources():
    libvalkey_sources = (
        "alloc",
        "async",
        "conn",
        "dict",
        "net",
        "read",
        "sds",
        "sockcompat",
        "valkey",
    )
    return sorted(
        glob.glob("src/*.c")
        + ["vendor/libvalkey/src/%s.c" % src for src in libvalkey_sources]
    )


def get_linker_args():
    if "win32" in sys.platform or "darwin" in sys.platform:
        return []
    else:
        return ["-Wl,-Bsymbolic"]


def get_compiler_args():
    if "win32" in sys.platform:
        return []
    else:
        return ["-std=c99"]


def get_libraries():
    if "win32" in sys.platform:
        return ["ws2_32"]
    else:
        return []


ext = Extension(
    "libvalkey.libvalkey",
    sources=get_sources(),
    extra_compile_args=get_compiler_args(),
    extra_link_args=get_linker_args(),
    libraries=get_libraries(),
    include_dirs=[
        "vendor/libvalkey/include",
        "vendor/libvalkey/include/valkey",
        # We need to include the src directory because we are building libvalkey
        # from source and not using the pre-built library. Therefore, we need to
        # add the internal dependencies.
        "vendor/libvalkey/src",
    ],
)

setup(
    name="libvalkey",
    version=version(),
    description="Python wrapper for libvalkey",
    long_description=io.open("README.md", "rt", encoding="utf-8").read(),
    long_description_content_type="text/markdown",
    url="https://github.com/valkey-io/libvalkey-py",
    author="libvalkey-py authors",
    author_email="libvalkey-py@lists.valkey.io",
    keywords=["Valkey"],
    license="MIT",
    packages=["libvalkey"],
    package_data={"libvalkey": ["libvalkey.pyi", "py.typed"]},
    ext_modules=[ext],
    python_requires=">=3.8",
    project_urls={
        "Changes": "https://github.com/valkey-io/libvalkey-py/releases",
        "Issue tracker": "https://github.com/valkey-io/libvalkey-py/issues",
    },
    classifiers=[
        "Development Status :: 5 - Production/Stable",
        "Intended Audience :: Developers",
        "License :: OSI Approved :: MIT License",
        "Operating System :: MacOS",
        "Operating System :: POSIX",
        "Programming Language :: C",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3 :: Only",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Python :: 3.13",
        "Programming Language :: Python :: Implementation :: CPython",
        "Topic :: Software Development",
    ],
)
