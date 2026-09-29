"""Small nonblocking event fan-out primitives for the emulator."""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator
from typing import Generic, TypeVar


EventT = TypeVar("EventT")
_CLOSED = object()


class SubscriptionClosed(RuntimeError):
    """Raised when a consumer reads from a disconnected subscription."""


class EventSubscription(Generic[EventT]):
    """One broker subscription with a bounded, private event queue."""

    def __init__(self, broker: EventBroker[EventT], max_queue_size: int) -> None:
        self._broker = broker
        self._queue: asyncio.Queue[EventT | object] = asyncio.Queue(maxsize=max_queue_size)
        self._closed = False

    @property
    def closed(self) -> bool:
        """Whether this subscription has been unsubscribed or overflowed."""

        return self._closed

    async def get(self) -> EventT:
        """Wait for the next event, or raise when the subscription is closed."""

        if self._closed and self._queue.empty():
            raise SubscriptionClosed("subscription is closed")
        item = await self._queue.get()
        if item is _CLOSED:
            self._queue.put_nowait(_CLOSED)
            raise SubscriptionClosed("subscription is closed")
        return item  # type: ignore[return-value]

    async def unsubscribe(self) -> None:
        """Remove this subscription.  It is safe to call more than once."""

        await self._broker.unsubscribe(self)

    async def __aenter__(self) -> EventSubscription[EventT]:
        return self

    async def __aexit__(self, *unused: object) -> None:
        await self.unsubscribe()

    def __aiter__(self) -> AsyncIterator[EventT]:
        return self._iterate()

    async def _iterate(self) -> AsyncIterator[EventT]:
        while True:
            try:
                yield await self.get()
            except SubscriptionClosed:
                return

    def _disconnect(self) -> None:
        if self._closed:
            return
        self._closed = True
        # Dropping buffered items lets a disconnected client observe closure
        # immediately and prevents one slow reader from retaining stale events.
        while not self._queue.empty():
            self._queue.get_nowait()
        self._queue.put_nowait(_CLOSED)


class EventBroker(Generic[EventT]):
    """Fan events out without allowing a slow subscriber to block publishers.

    Publishing is synchronous and never waits.  If a subscriber's bounded queue
    is full, that subscriber is disconnected and its buffered events are dropped.
    """

    def __init__(self, *, max_queue_size: int = 64) -> None:
        if not isinstance(max_queue_size, int) or isinstance(max_queue_size, bool) or max_queue_size < 1:
            raise ValueError("max_queue_size must be a positive integer")
        self.max_queue_size = max_queue_size
        self._subscriptions: set[EventSubscription[EventT]] = set()

    @property
    def subscriber_count(self) -> int:
        """The number of connected subscribers."""

        return len(self._subscriptions)

    async def subscribe(self, *, max_queue_size: int | None = None) -> EventSubscription[EventT]:
        """Create a connected subscription with an optionally smaller queue."""

        queue_size = self.max_queue_size if max_queue_size is None else max_queue_size
        if not isinstance(queue_size, int) or isinstance(queue_size, bool) or queue_size < 1:
            raise ValueError("max_queue_size must be a positive integer")
        subscription = EventSubscription(self, queue_size)
        self._subscriptions.add(subscription)
        return subscription

    async def unsubscribe(self, subscription: EventSubscription[EventT]) -> None:
        """Disconnect and remove a subscription owned by this broker."""

        if subscription._broker is not self:
            raise ValueError("subscription belongs to another broker")
        self._subscriptions.discard(subscription)
        subscription._disconnect()

    def publish(self, event: EventT) -> int:
        """Deliver *event* without waiting, returning its delivery count."""

        delivered = 0
        for subscription in tuple(self._subscriptions):
            if subscription._closed or subscription._queue.full():
                self._subscriptions.discard(subscription)
                subscription._disconnect()
                continue
            subscription._queue.put_nowait(event)
            delivered += 1
        return delivered
