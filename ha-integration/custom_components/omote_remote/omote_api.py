"""HTTP client for Omote OS device API."""

from __future__ import annotations

import aiohttp


class OmoteApi:
    """Minimal Omote REST client."""

    def __init__(self, host: str, session: aiohttp.ClientSession) -> None:
        self._host = host.rstrip("/")
        if not self._host.startswith("http"):
            self._host = f"http://{self._host}"
        self._session = session

    async def get_status(self) -> dict:
        async with self._session.get(
            f"{self._host}/api/status", timeout=aiohttp.ClientTimeout(total=8)
        ) as resp:
            resp.raise_for_status()
            return await resp.json()

    async def get_last_event(self) -> dict:
        async with self._session.get(
            f"{self._host}/api/event/last", timeout=aiohttp.ClientTimeout(total=8)
        ) as resp:
            resp.raise_for_status()
            return await resp.json()
