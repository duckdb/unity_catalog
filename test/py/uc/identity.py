"""Unified test identity: turn provisioned table refs into the env a body substitutes.

The contract (see WIP-identity-design.md): every provisioned requirement is addressable as
`{CATALOG}.{SCHEMA}.{TABLE}`, plus per-requirement namespaced vars `{KEY_CATALOG}` /
`{KEY_SCHEMA}` / `{KEY_TABLE}` and `{KEY}` (= full FQN). The *primary* (sole/first)
requirement additionally gets the bare `{CATALOG}` / `{SCHEMA}` / `{TABLE}` / `{ID_NAME}`
for the common single-table body.

Keys come from the `@requires` name (default: the bare table name), made env-safe
(uppercased, every non-alphanumeric char -> `_`, since env vars can't hold `.`/`-`).

This module is UC-local for now; it is the generic core that migrates into the driver's
`Provisioner` base (design step B), so it stays free of any backend/UC specifics.
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class TableRef:
    """One provisioned table, and the key a body addresses it by."""

    key: str  # the @requires key (name override, else the bare table name)
    catalog: str
    schema: str
    table: str
    access: str = "ro"

    @property
    def fqn(self) -> str:
        return f"{self.catalog}.{self.schema}.{self.table}"


def env_key(name: str) -> str:
    """Env-safe var key: uppercase, non-alphanumeric -> `_` (env vars can't hold `.`/`-`)."""
    return "".join(c if c.isalnum() else "_" for c in name).upper()


def build_env(refs, *, primary=None) -> dict:
    """Env dict a body substitutes `{VAR}` from.

    refs    : iterable of TableRef (one per provisioned requirement).
    primary : the TableRef (or its key) that also gets the bare CATALOG/SCHEMA/TABLE +
              its FQN; defaults to the first ref.
    """
    refs = list(refs)
    env = {}
    for r in refs:
        k = env_key(r.key)
        env[f"{k}_CATALOG"] = r.catalog
        env[f"{k}_SCHEMA"] = r.schema
        env[f"{k}_TABLE"] = r.table
        env[k] = r.fqn  # the `{ID_NAME}`-style FQN alias
    if refs:
        if not isinstance(primary, TableRef):
            primary = next((r for r in refs if r.key == primary), refs[0])
        env["CATALOG"] = primary.catalog
        env["SCHEMA"] = primary.schema
        env["TABLE"] = primary.table
    return env
