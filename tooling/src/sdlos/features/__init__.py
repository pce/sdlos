"""sdlos.features — optional feature blocks applied during app scaffolding.

Each feature is a self-contained class with an ``apply(context)`` method.
The ``create`` command collects the active features from AppConfig and
calls them in order after the core scaffold files have been written.

Available features
------------------
Bundler   — packs build output into a distributable archive.
Updater   — generates and pre-configures an auto-updater binary stub.
"""

from .bundler import Bundler
from .updater import Updater

__all__ = ["Bundler", "Updater"]
