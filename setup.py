"""
Setup script for RDMA KV Cache
Builds C++ extensions and installs Python package
"""

import os
import sys
import subprocess
from pathlib import Path
from setuptools import setup, Extension, find_packages
from setuptools.command.build_ext import build_ext


class CMakeExtension(Extension):
    """Custom extension that uses CMake to build"""

    def __init__(self, name, sourcedir=''):
        Extension.__init__(self, name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir)


class CMakeBuild(build_ext):
    """Custom build_ext command that invokes CMake"""

    def run(self):
        # Check CMake is installed
        try:
            subprocess.check_output(['cmake', '--version'])
        except OSError:
            raise RuntimeError(
                "CMake must be installed to build the following extensions: " +
                ", ".join(e.name for e in self.extensions)
            )

        for ext in self.extensions:
            self.build_extension(ext)

    def build_extension(self, ext):
        extdir = os.path.abspath(os.path.dirname(self.get_ext_fullpath(ext.name)))

        # CMake configure arguments
        cmake_args = [
            f'-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={extdir}',
            f'-DPYTHON_EXECUTABLE={sys.executable}',
        ]

        # Build type
        cfg = 'Debug' if self.debug else 'Release'
        build_args = ['--config', cfg]

        # Platform-specific configuration
        if sys.platform.startswith('darwin'):
            # macOS specific settings
            cmake_args += [
                '-DCMAKE_OSX_DEPLOYMENT_TARGET=10.14',
            ]

        cmake_args += [f'-DCMAKE_BUILD_TYPE={cfg}']
        build_args += ['--', '-j4']  # Parallel build

        env = os.environ.copy()
        env['CXXFLAGS'] = '{} -DVERSION_INFO=\\"{}\\"'.format(
            env.get('CXXFLAGS', ''),
            self.distribution.get_version()
        )

        # Create build directory
        if not os.path.exists(self.build_temp):
            os.makedirs(self.build_temp)

        # Run CMake configure
        print(f"Running CMake configure in {self.build_temp}")
        subprocess.check_call(
            ['cmake', ext.sourcedir] + cmake_args,
            cwd=self.build_temp,
            env=env
        )

        # Run CMake build
        print(f"Running CMake build")
        subprocess.check_call(
            ['cmake', '--build', '.'] + build_args,
            cwd=self.build_temp
        )


# Read README
def read_readme():
    readme_path = Path(__file__).parent / "README.md"
    if readme_path.exists():
        return readme_path.read_text(encoding='utf-8')
    return ""


# Read requirements
def read_requirements():
    req_path = Path(__file__).parent / "requirements.txt"
    if req_path.exists():
        with open(req_path) as f:
            return [line.strip() for line in f if line.strip() and not line.startswith('#')]
    return []


setup(
    name='rdma-kv-cache',
    version='0.1.0',
    author='RDMA KV Cache Team',
    author_email='',
    description='RDMA-based KV cache transfer for disaggregated LLM inference',
    long_description=read_readme(),
    long_description_content_type='text/markdown',
    url='https://github.com/yourusername/rdma-kv-cache',

    # Python packages
    packages=find_packages(where='python'),
    package_dir={'': 'python'},

    # C++ extensions
    ext_modules=[CMakeExtension('rdma_kv_bindings')],
    cmdclass={'build_ext': CMakeBuild},

    # Dependencies
    install_requires=read_requirements(),

    # Optional dependencies
    extras_require={
        'api': ['fastapi>=0.100.0', 'uvicorn>=0.23.0'],
        'dev': ['pytest>=7.0.0', 'black>=23.0.0', 'mypy>=1.0.0'],
        'all': ['fastapi>=0.100.0', 'uvicorn>=0.23.0', 'pytest>=7.0.0'],
    },

    # Entry points for vLLM plugin system
    entry_points={
        'vllm.general_plugins': [
            'rdma_connector = rdma_connector:register_plugin'
        ],
        'console_scripts': [
            'rdma-orchestrator=orchestrator.server:main',
            'rdma-orchestrator-api=orchestrator.api:main',
            'rdma-client=client.client:main',
        ],
    },

    # Classifiers
    classifiers=[
        'Development Status :: 3 - Alpha',
        'Intended Audience :: Developers',
        'Intended Audience :: Science/Research',
        'Topic :: Scientific/Engineering :: Artificial Intelligence',
        'License :: OSI Approved :: Apache Software License',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python :: 3.8',
        'Programming Language :: Python :: 3.9',
        'Programming Language :: Python :: 3.10',
        'Programming Language :: Python :: 3.11',
        'Programming Language :: C++',
    ],

    python_requires='>=3.8',
    zip_safe=False,
)
