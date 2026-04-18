# Installation

## Requirements

- Python **3.10+**
- Linux or macOS (Windows works but without `uvloop`)
- ZeroMQ shared library (bundled via `pyzmq`)

## Install from source

```bash
git clone https://github.com/sudoRicheek/cortex.git
cd cortex
pip install -e ".[dev]"
```

## Optional extras

=== "PyTorch support"

    ```bash
    pip install -e ".[torch]"
    ```

    Enables [`TensorMessage`][cortex.messages.standard.TensorMessage] and
    torch-aware serialization paths.

=== "Everything"

    ```bash
    pip install -e ".[all]"
    ```

## Verify

```python
import cortex
print(cortex.__version__)
```

If that prints a version string, you're ready. Continue to the
[Quickstart](quickstart.md).
