"""
Shared logging configuration for Cortex.

Provides colored console logging with consistent formatting across all modules.
"""

import logging
import sys

# ANSI color codes (no external dependencies)
RESET = "\033[0m"
BOLD = "\033[1m"

# Foreground colors
BLACK = "\033[30m"
RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
BLUE = "\033[34m"
MAGENTA = "\033[35m"
CYAN = "\033[36m"
WHITE = "\033[37m"

# Bright foreground colors
BRIGHT_RED = "\033[91m"
BRIGHT_GREEN = "\033[92m"
BRIGHT_YELLOW = "\033[93m"
BRIGHT_BLUE = "\033[94m"
BRIGHT_MAGENTA = "\033[95m"
BRIGHT_CYAN = "\033[96m"


class ColoredFormatter(logging.Formatter):
    """
    Custom formatter with colors for different log levels and message types.
    """

    LEVEL_COLORS = {
        "DEBUG": CYAN,
        "INFO": GREEN,
        "WARNING": YELLOW,
        "ERROR": RED,
        "CRITICAL": BRIGHT_RED,
    }

    def format(self, record: logging.LogRecord) -> str:
        # Color the level name
        levelname = record.levelname
        if levelname in self.LEVEL_COLORS:
            record.levelname = f"{self.LEVEL_COLORS[levelname]}{levelname:<5}{RESET}"

        # Format the message first
        formatted = super().format(record)

        # Restore original levelname for potential reuse
        record.levelname = levelname

        # Apply message-specific colors
        msg = record.getMessage()

        # Blue separators
        if msg.startswith("-") or msg.startswith("="):
            return self._colorize_line(formatted, BLUE)

        return formatted

    def _colorize_line(self, line: str, color: str, bold: bool = False) -> str:
        """Apply color to the message portion of the log line."""
        # Find the last | separator and colorize everything after it
        parts = line.rsplit("|", 1)
        if len(parts) == 2:
            prefix, message = parts
            if bold:
                return f"{prefix}|{color}{BOLD}{message}{RESET}"
            return f"{prefix}|{color}{message}{RESET}"
        if bold:
            return f"{color}{BOLD}{line}{RESET}"
        return f"{color}{line}{RESET}"


def get_logger(name: str, level: int = logging.INFO) -> logging.Logger:
    """
    Get a configured logger with colored output.

    Args:
        name: Logger name (e.g., "cortex.discovery")
        level: Logging level (default: INFO)

    Returns:
        Configured logger instance
    """
    logger = logging.getLogger(name)

    # Avoid adding duplicate handlers
    if logger.handlers:
        return logger

    logger.setLevel(level)
    logger.propagate = False

    # Create console handler with colored formatter
    handler = logging.StreamHandler(sys.stdout)
    handler.setLevel(level)
    handler.setFormatter(
        ColoredFormatter(
            fmt="%(asctime)s | %(levelname)s | %(message)s",
            datefmt="%H:%M:%S",
        )
    )

    logger.addHandler(handler)

    return logger


def set_log_level(logger: logging.Logger, level: str) -> None:
    """
    Set the log level for a logger and its handlers.

    Args:
        logger: The logger to configure
        level: Log level name ("DEBUG", "INFO", "WARNING", "ERROR")
    """
    log_level = getattr(logging, level.upper(), logging.INFO)
    logger.setLevel(log_level)
    for handler in logger.handlers:
        handler.setLevel(log_level)
