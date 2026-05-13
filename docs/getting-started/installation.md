# Installation

## Requirements

- Python **3.10+**
- Linux or macOS (Windows works without `uvloop`)
- ZeroMQ shared library (bundled via `pyzmq`)

## From source

```bash
git clone https://github.com/sudoRicheek/cortex.git
cd cortex
pip install -e ".[dev]"
```

## Optional extras

```bash
pip install -e ".[torch]"   # TensorMessage + torch-aware serialization
pip install -e ".[all]"     # everything
```

## Verify

```python
import cortex
print(cortex.__version__)
```

If that prints a version string, continue to the [Quickstart](quickstart.md).
