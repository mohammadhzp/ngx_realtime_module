import asyncio
import socket


class SysLogProtocol(asyncio.DatagramProtocol):
    def datagram_received(self, data, addr):
        try:
            self.run(data, addr)
        except Exception as e:
            print(e)

    def run(self, data, addr):
        final = dict()
        for info in data.decode().split("|"):
            k, v = info.split(':', 3)[-1].split("!#!")
            final[k.strip()] = v.strip()

        print(f'{addr}:', final)


async def run():
    loop = asyncio.get_event_loop()
    transport, protocol = await loop.create_datagram_endpoint(
        SysLogProtocol, family=socket.AF_INET, local_addr=('127.0.0.1', 5145))

    try:
        await asyncio.Event().wait()

    finally:
        transport.close()


if __name__ == '__main__':
    try:
        asyncio.run(run())

    except KeyboardInterrupt:
        pass
