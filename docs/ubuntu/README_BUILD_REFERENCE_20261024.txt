Historical README retained for reference. Use the current README for the supported new-chain build profile.

# PureEOS

本项目独立维护并决定后续技术路线。自有原创部分的许可见
[AIC 开源许可 1.0](AIC_LICENSE.md)，与继承代码的许可边界见
[LICENSING.md](LICENSING.md)。现有链 LICENSE 中的 BSL 适用范围仍需核查，
新增 AIC 许可不表示全仓库已重新授权。

1. [Branches](#branches)
2. [Supported Operating Systems](#supported-operating-systems)
3. [Binary Installation](#binary-installation)
4. [Build and Install from Source](#build-and-install-from-source)
5. [Bash Autocomplete](#bash-autocomplete)

PureEOS is an independently maintained C++ blockchain project with native Savanna consensus and experimental transaction parallel execution. This repository contains the node and its operator/developer tools. AIC CDT 1.0.0 is maintained separately and will be released with the first stable parallel version.

## Branches
The `main` branch is a development branch. The first stable parallel release is still being tested; the default branch, historical binaries and development refs do not certify the current candidate. See the [current acceptance status](docs/parallel-execution/PX7_V1_CURRENT_STATUS.txt) and [release checklist](docs/parallel-execution/PX7_V1_RELEASE_CHECKLIST.txt).

The candidate CI entry checks tooling and native components in Ubuntu 26.04/26.10 user spaces. Its [scope and remaining delivery gates](docs/parallel-execution/PX7_V1_CI_DELIVERY_REPAIR_20261024.md) are explicit; a green component job does not certify a stable joint release. The current private VM development dependency still requires access until the public source gate is completed.

## Supported Operating Systems
The new-chain parallel JIT profile targets **Ubuntu 26.04 LTS and Ubuntu 26.10, x86_64**.
Support and verification are scoped as follows (2026-10-23):

| Platform | Profile | Verification |
|---|---|---|
| Ubuntu 26.04 LTS | Primary native build / JIT | Full native Release node and producer-plugin builds; queue, timer, producer and Savanna restart checks passed. |
| Ubuntu 26.10 | Current interim release / JIT | Exact-source node/tool build and core regression passed. Two native cloud VMs completed 30→40 candidate rotation and offline-window blacklist checks; an ABI boolean completion-check defect was corrected and original evidence independently revalidated. Real-day payouts, VM reboot recovery, current sanitizer and delivery checks remain open. TPS testing follows first publication. |

See the [modern Ubuntu verification report](docs/ubuntu/UBUNTU_MODERN_RESULTS.txt) for exact tools,
test scope and remaining gaps. The 26.10 container tests share the host kernel; they do not certify
a fresh 26.10 kernel installation. Existing Ubuntu 22.04 CI and Ubuntu 24.04 cloud results remain
historical references, not substitutes for testing this new profile. No new release package is implied.

Ubuntu 26.10 is the current interim release according to the [official release announcement](https://lists.ubuntu.com/archives/ubuntu-announce/2026-October/000330.html);
26.04 is the current [LTS series](https://documentation.ubuntu.com/release-notes/26.04/).

Other Unix derivatives such as macOS are tended to on a best-effort basis and may not be full featured. If you aren't using Ubuntu, please visit the "[Build Unsupported OS](./docs/00_install/01_build-from-source/00_build-unsupported-os.md)" page to explore your options.

If you are running an unsupported Ubuntu derivative, such as Linux Mint, you can find the version of Ubuntu your distribution was based on by using this command:
```bash
cat /etc/upstream-release/lsb-release
```
Your best bet is to follow the instructions for your Ubuntu base, but we make no guarantees.

## Binary Installation
The first stable parallel runtime and AIC CDT 1.0.0 have not been jointly released.
Do not treat a development archive or an older release as this candidate. The
[current candidate guide](docs/parallel-execution/PX7_V1_CANDIDATE_GUIDE.txt) explains
its scope; [current acceptance status](docs/parallel-execution/PX7_V1_CURRENT_STATUS.txt)
records what passed and what remains.

The tested runtime reports `v2.0.0` and an exact source-manifest identifier through
`nodeos --full-version`. That identifier describes the actual archived build
source, including its dependencies; it is not a stable release tag or an invented
Git commit. Final packages will include their own SHA256/provenance manifests.

## Build and Install from Source
You can build PureEOS from source.

### Prerequisites
You will need to build on a [supported operating system](#supported-operating-systems).

Requirements to build:
- C++20 compiler and standard library
- CMake 3.21+
- Modern Ubuntu JIT profile: no separate LLVM development package is required.
  - The optional `eos-vm-oc` backend still requires LLVM 7–11 and is disabled in this profile.
  - Installing a newer LLVM does not make that backend supported.
- libcurl 7.40.0+
- git
- GMP
- Python 3
- python3-numpy
- zlib

For an isolated modern Ubuntu build, use:

```bash
bash tools/ubuntu-modern-build.sh --check
bash tools/ubuntu-modern-build.sh --build --jobs=2
bash tools/ubuntu-modern-build.sh --test --jobs=2
```

The script uses a separate build directory and the executable name `pureeos-ubuntu-nodeos`,
refuses unrelated existing build caches, and does not install packages or replace CDT.
It uses Ninja when available, otherwise Make. `--test` runs targeted functional checks, not full
release or TPS acceptance. Parallel execution still requires its explicit runtime configuration.

### Step 1 - Clone
Open a terminal and choose a directory for the PureEOS source:
```bash
cd ~/Downloads
```
Clone PureEOS using either HTTPS...
```bash
git clone https://github.com/MatrixCoreX/PureEOS.git
```
...or SSH:
```bash
git clone git@github.com:MatrixCoreX/PureEOS.git
```

> ℹ️ **HTTPS vs. SSH Clone** ℹ️  
Both an HTTPS or SSH git clone will yield the same result - a folder named `PureEOS` containing our source code. It doesn't matter which type you use.

Navigate into that folder:
```bash
cd PureEOS
```

### Step 2 - Checkout Release Tag or Branch
The current candidate is a development version. Select the intended development
ref explicitly; no stable tag is announced by this README. The exact build-source
archive is independent of a newer working tree and remains the reference for
reproducing the tested binaries.

Initialize the pinned dependencies only after choosing your ref:

```bash
git submodule update --init --recursive
python3 -I -B tools/prepare_vm_vendor.py --apply
python3 -I -B tools/prepare_vm_vendor.py --check
```

The PureEOS VM development repository is currently private. Its relative URL
resolves beside this repository, so use SSH or authenticated HTTPS with access to
`MatrixCoreX/PureEOS-VM`. The first public release still requires a public-access
clean checkout check. The fixed dependency commits and the exact three-header
patch preserve the tested VM bytes; initialization does not upgrade dependencies
to their latest branches. The patch tool refuses unknown edits and never resets
or stages a checkout. Complete source archives already contain the prepared
vendor files and support the read-only `--check` path without Git metadata.

Select build instructions below for a [pinned build](#pinned-build) (preferred) or an [unpinned build](#unpinned-build).

> ℹ️ **Pinned vs. Unpinned Build** ℹ️  
We have two types of builds for Helios: "pinned" and "unpinned." A pinned build is a reproducible build with the build environment and dependency versions fixed by the development team. In contrast, unpinned builds use the dependency versions provided by the build platform. Unpinned builds tend to be quicker because the pinned build environment must be built from scratch. Pinned builds, in addition to being reproducible, ensure the compiler remains the same between builds of different Helios major versions. Helios requires the compiler version to remain the same, otherwise its state might need to be recovered from a portable snapshot or the chain needs to be replayed.

> ⚠️ **A Warning On Parallel Compilation Jobs (`-j` flag)** ⚠️  
When building C/C++ software, often the build is performed in parallel via a command such as `make -j "$(nproc)"` which uses all available CPU threads. However, be aware that some compilation units (`*.cpp` files) in Helios will consume nearly 4GB of memory. Failures due to memory exhaustion will typically, but not always, manifest as compiler crashes. Using all available CPU threads may also prevent you from doing other things on your computer during compilation. For these reasons, consider reducing this value.

> 🐋 **Docker and `sudo`** 🐋  
If you are in an Ubuntu docker container, omit `sudo` from all commands because you run as `root` by default. Most other docker containers also exclude `sudo`, especially Debian-family containers. If your shell prompt is a hash tag (`#`), omit `sudo`.

#### Pinned Reproducible Build
The pinned reproducible build requires Docker. Make sure you are in the root of the `helios` repo and then run
```bash
DOCKER_BUILDKIT=1 docker build -f tools/reproducible.Dockerfile -o . .
```
This command will take a substantial amount of time because a toolchain is built from scratch. Upon completion, the current directory will contain a built `.deb` and `.tar.gz` (you can change the `-o .` argument to place the output in a different directory). If needing to reduce the number of parallel jobs as warned above, run the command as,
```bash
DOCKER_BUILDKIT=1 docker build --build-arg HELIOS_BUILD_JOBS=4 -f tools/reproducible.Dockerfile -o . .
```

#### Unpinned Build
The following instructions are valid for this branch. Other release branches may have different requirements, so ensure you follow the directions in the branch or release you intend to build. If you are in an Ubuntu docker container, omit `sudo` because you run as `root` by default.

Install dependencies:
```bash
sudo apt-get update
sudo apt-get install -y \
        build-essential \
        cmake \
        git \
        libcurl4-openssl-dev \
        libgmp-dev \
        ninja-build \
        pkg-config \
        python3-venv \
        python3-numpy \
        file \
        zlib1g-dev
```

To build, make sure you are in the root of the `helios` repo, then run the following command:
```bash
mkdir -p build
cd build

cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_OC=OFF ..

make -j "$(nproc)" package
```

Now you can optionally [test](#step-4---test) your build, or [install](#step-5---install) the `*.deb` binary packages, which will be in the root of your build directory.

### Step 4 - Test
Helios supports the following test suites:

Test Suite | Test Type | [Test Size](https://testing.googleblog.com/2010/12/test-sizes.html) | Notes
---|:---:|:---:|---
[Parallelizable tests](#parallelizable-tests) | Unit tests | Small
[WASM spec tests](#wasm-spec-tests) | Unit tests | Small | Unit tests for our WASM runtime, each short but _very_ CPU-intensive
[Serial tests](#serial-tests) | Component/Integration | Medium
[Long-running tests](#long-running-tests) | Integration | Medium-to-Large | Tests which take an extraordinarily long amount of time to run

When building from source, we recommended running at least the [parallelizable tests](#parallelizable-tests).

#### Parallelizable Tests
This test suite consists of any test that does not require shared resources, such as file descriptors, specific folders, or ports, and can therefore be run concurrently in different threads without side effects (hence, easily parallelized). These are mostly unit tests and [small tests](https://testing.googleblog.com/2010/12/test-sizes.html) which complete in a short amount of time.

You can invoke them by running `ctest` from a terminal in your Helios build directory and specifying the following arguments:
```bash
ctest -j "$(nproc)" -LE _tests
```

#### WASM Spec Tests
The WASM spec tests verify that our WASM execution engine is compliant with the web assembly standard. These are very [small](https://testing.googleblog.com/2010/12/test-sizes.html), very fast unit tests. However, there are over a thousand of them so the suite can take a little time to run. These tests are extremely CPU-intensive.

You can invoke them by running `ctest` from a terminal in your Helios build directory and specifying the following arguments:
```bash
ctest -j "$(nproc)" -L wasm_spec_tests
```
We have observed severe performance issues when multiple virtual machines are running this test suite on the same physical host at the same time, for example in a CICD system. This can be resolved by disabling hyperthreading on the host.

#### Serial Tests
The serial test suite consists of [medium](https://testing.googleblog.com/2010/12/test-sizes.html) component or integration tests that use specific paths, ports, rely on process names, or similar, and cannot be run concurrently with other tests. Serial tests can be sensitive to other software running on the same host and they may `SIGKILL` other `nodeos` processes. These tests take a moderate amount of time to complete, but we recommend running them.

You can invoke them by running `ctest` from a terminal in your Helios build directory and specifying the following arguments:
```bash
ctest -L "nonparallelizable_tests"
```

#### Long-Running Tests
The long-running tests are [medium-to-large](https://testing.googleblog.com/2010/12/test-sizes.html) integration tests that rely on shared resources and take a very long time to run.

You can invoke them by running `ctest` from a terminal in your Helios build directory and specifying the following arguments:
```bash
ctest -L "long_running_tests"
```

### Step 5 - Install
Once you have [built](#step-3---build-the-source-code) Helios and [tested](#step-4---test) your build, you can install Helios on your system. Don't forget to omit `sudo` if you are running in a docker container.

We recommend installing the binary package you just built. Navigate to your Helios build directory in a terminal and run this command:
```bash
sudo apt-get update
sudo apt-get install -y ./helios_*.deb
```

It is also possible to install using `make` instead:
```bash
sudo make install
```

## Bash Autocomplete
`cleos` and `helios-util` offer a substantial amount of functionality. Consider using bash's autocompletion support which makes it easier to discover all their various options.

For our provided `.deb` packages simply install Ubuntu's `bash-completion` package: `apt-get install bash-completion` (you may need to log out/in after installing).

If building from source install the `build/programs/cleos/bash-completion/completions/cleos` and `build/programs/helios-util/bash-completion/completions/helios-util` files to your bash-completion directory. Refer to [bash-completion's documentation](https://github.com/scop/bash-completion#faq) on the possible install locations.
