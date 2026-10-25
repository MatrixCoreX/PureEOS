"""Checkout fixed dependencies, with an optional read-only key for the VM mirror."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


class DependencyGitError(RuntimeError):
    """A fixed diagnostic category; never reproduce credential-bearing stderr."""


def git(root, *args, env=None):
    if env is None:
        env = {k: v for k, v in os.environ.items() if k != "AIC_VM_DEPLOY_KEY"}
    try:
        result = subprocess.run(["git", "-C", str(root), *args], check=True,
                                capture_output=True, text=True, env=env, timeout=1800)
    except subprocess.CalledProcessError as error:
        categories = (("dubious ownership", "unsafe-worktree-owner"),
                      ("Permission denied (publickey)", "ssh-read-permission"),
                      ("Repository not found", "repository-read-permission"),
                      ("Could not resolve host", "dns-resolution"),
                      ("Host key verification failed", "ssh-host-key"))
        category = next((name for marker, name in categories if marker in (error.stderr or "")),
                        "unclassified-git-error")
        raise DependencyGitError("Dependency checkout failed: " + category) from None
    return result.stdout.strip()


@contextmanager
def vm_key_environment(key, variable, fetch_keys=None):
    # The key exists only outside the checkout and is never passed on argv.
    begin = "-----BEGIN " + "OPENSSH PRIVATE KEY-----"
    end = "-----END " + "OPENSSH PRIVATE KEY-----"
    if not key.startswith(begin) or not key.rstrip().endswith(end):
        raise ValueError("Expected an OpenSSH deployment key")
    if fetch_keys is None:
        def fetch_keys():
            inventory = Path(__file__).resolve().parent / "vendor/github-ssh-host-keys.json"
            return json.loads(inventory.read_text())["ssh_keys"]
    keys = fetch_keys()
    if not keys or any("\n" in item or "\r" in item or len(item.split()) != 2
                       or item.split()[0] not in ("ssh-rsa", "ssh-ed25519", "ecdsa-sha2-nistp256")
                       for item in keys):
        raise ValueError("Invalid official GitHub SSH host-key inventory")
    with tempfile.TemporaryDirectory(prefix="aic-ci-vm-key-") as directory:
        directory = Path(directory)
        identity, known = directory / "identity", directory / "known_hosts"
        identity.write_text(key.rstrip() + "\n")
        identity.chmod(0o600)
        known.write_text("".join("github.com " + item + "\n" for item in keys))
        known.chmod(0o600)
        env = {k: v for k, v in os.environ.items() if k != variable}
        env["GIT_SSH_COMMAND"] = shlex.join([
            "ssh", "-F", "/dev/null", "-i", str(identity), "-o", "IdentitiesOnly=yes",
            "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
            "-o", "UserKnownHostsFile=" + str(known)])
        yield env


def checkout(root, expected, key=None, variable="AIC_VM_DEPLOY_KEY", progress=None):
    root = Path(root).resolve(strict=True)
    def note(message):
        if progress is not None:
            progress(message)
    note("Initializing fixed VM dependency")
    git(root, "submodule", "init", "libraries/eos-vm")
    note("Fetching fixed VM dependency and its public dependencies")
    if key:
        with vm_key_environment(key, variable) as env:
            git(root, "config", "submodule.libraries/eos-vm.url",
                "git@github.com:MatrixCoreX/PureEOS-VM.git", env=env)
            git(root, "submodule", "update", "--init", "--recursive", "--jobs=4", "libraries/eos-vm", env=env)
    else:
        git(root, "submodule", "update", "--init", "--recursive", "--jobs=4", "libraries/eos-vm")
    actual = git(root / "libraries/eos-vm", "rev-parse", "HEAD")
    if actual != expected:
        raise RuntimeError("VM checkout differs from the fixed source commit")
    note("Fixed VM verified; fetching remaining public dependencies with four jobs")
    # All other roots and the VM's nested dependencies use their public URLs.
    git(root, "submodule", "update", "--init", "--recursive", "--jobs=4")
    status = git(root, "submodule", "status", "--recursive")
    if any(line.startswith(("-", "+", "U")) for line in status.splitlines()):
        raise RuntimeError("Dependency checkout is incomplete or differs from fixed gitlinks")
    return {"status": "fixed-recursive-dependencies-checked-out", "vm_commit": actual,
            "VM_deployment_key_used": bool(key), "recursive_modules": len(status.splitlines())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    expected = json.loads((args.root / "tools/vendor/eos-vm-parallel-v1.json").read_text())["base_commit"]
    try:
        result = checkout(args.root, expected, os.environ.get("AIC_VM_DEPLOY_KEY"),
                          progress=lambda message: print(message, flush=True))
    except DependencyGitError as error:
        parser.exit(1, str(error) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
