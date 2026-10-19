"""EventBroker fan-out, subscription closure and slow-subscriber overflow."""

from __future__ import annotations

import asyncio
import unittest

from aula_emulator.events import EventBroker, SubscriptionClosed


class EventBrokerTest(unittest.IsolatedAsyncioTestCase):
    async def test_publish_reaches_all_subscribers_and_counts(self) -> None:
        broker: EventBroker[str] = EventBroker()
        first = await broker.subscribe()
        second = await broker.subscribe()
        self.assertEqual(broker.subscriber_count, 2)
        self.assertEqual(broker.publish("a"), 2)
        self.assertEqual(await first.get(), "a")
        self.assertEqual(await second.get(), "a")

    async def test_publish_without_subscribers_delivers_zero(self) -> None:
        self.assertEqual(EventBroker[str]().publish("a"), 0)

    async def test_events_keep_order(self) -> None:
        broker: EventBroker[int] = EventBroker()
        subscription = await broker.subscribe()
        for value in (1, 2, 3):
            broker.publish(value)
        self.assertEqual([await subscription.get() for _ in range(3)], [1, 2, 3])

    async def test_unsubscribe_disconnects(self) -> None:
        broker: EventBroker[str] = EventBroker()
        subscription = await broker.subscribe()
        await subscription.unsubscribe()
        await subscription.unsubscribe()  # idempotent
        self.assertTrue(subscription.closed)
        self.assertEqual(broker.subscriber_count, 0)
        self.assertEqual(broker.publish("a"), 0)
        with self.assertRaises(SubscriptionClosed):
            await subscription.get()

    async def test_unsubscribe_drops_buffered_events(self) -> None:
        broker: EventBroker[str] = EventBroker()
        subscription = await broker.subscribe()
        broker.publish("a")
        await subscription.unsubscribe()
        with self.assertRaises(SubscriptionClosed):
            await subscription.get()

    async def test_async_context_manager_unsubscribes(self) -> None:
        broker: EventBroker[str] = EventBroker()
        async with await broker.subscribe() as subscription:
            broker.publish("a")
            self.assertEqual(await subscription.get(), "a")
        self.assertTrue(subscription.closed)
        self.assertEqual(broker.subscriber_count, 0)

    async def test_async_iteration_ends_on_close(self) -> None:
        broker: EventBroker[int] = EventBroker()
        subscription = await broker.subscribe()
        broker.publish(1)
        broker.publish(2)
        received: list[int] = []

        async def consume() -> None:
            async for event in subscription:
                received.append(event)
                if len(received) == 2:
                    await subscription.unsubscribe()

        await asyncio.wait_for(consume(), timeout=1)
        self.assertEqual(received, [1, 2])

    async def test_overflow_disconnects_only_the_slow_subscriber(self) -> None:
        broker: EventBroker[int] = EventBroker(max_queue_size=2)
        slow = await broker.subscribe()
        fast = await broker.subscribe(max_queue_size=3)
        self.assertEqual(broker.publish(1), 2)
        self.assertEqual(broker.publish(2), 2)
        self.assertEqual(await fast.get(), 1)
        self.assertEqual(await fast.get(), 2)
        # The slow queue (size 2) is full: it is dropped, the fast one is not.
        self.assertEqual(broker.publish(3), 1)
        self.assertTrue(slow.closed)
        self.assertFalse(fast.closed)
        self.assertEqual(broker.subscriber_count, 1)
        with self.assertRaises(SubscriptionClosed):
            await slow.get()  # buffered events were dropped
        self.assertEqual(await fast.get(), 3)

    async def test_closed_subscription_keeps_raising(self) -> None:
        broker: EventBroker[int] = EventBroker()
        subscription = await broker.subscribe()
        await subscription.unsubscribe()
        for _ in range(2):
            with self.assertRaises(SubscriptionClosed):
                await subscription.get()

    async def test_foreign_subscription_is_rejected(self) -> None:
        other: EventBroker[int] = EventBroker()
        subscription = await other.subscribe()
        with self.assertRaises(ValueError):
            await EventBroker[int]().unsubscribe(subscription)
        self.assertFalse(subscription.closed)

    async def test_invalid_queue_sizes(self) -> None:
        for size in (0, -1, True, 1.5):
            with self.subTest(size=size):
                with self.assertRaises(ValueError):
                    EventBroker(max_queue_size=size)  # type: ignore[arg-type]
                with self.assertRaises(ValueError):
                    await EventBroker[int]().subscribe(max_queue_size=size)  # type: ignore[arg-type]


if __name__ == "__main__":
    unittest.main()
