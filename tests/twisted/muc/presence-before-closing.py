"""
Test for fd.o#19930.
"""

import dbus

from twisted.words.xish import domish

from gabbletest import exec_test, make_result_iq
from servicetest import (EventPattern, assertEquals, assertLength,
        assertContains, sync_dbus, call_async)
import constants as cs
import ns

from mucutil import join_muc, echo_muc_presence

def test(q, bus, conn, stream):
    conn.Connect()
    q.expect('dbus-signal', signal='StatusChanged',
            args=[cs.CONN_STATUS_CONNECTED, cs.CSR_REQUESTED])

    room = 'test@conf.localhost'

    room_handle, chan, path, props, disco = join_muc(q, bus, conn, stream,
            room,
            also_capture=[EventPattern('stream-iq', iq_type='get',
                query_name='query', query_ns=ns.DISCO_INFO, to=room)])

    sync_dbus(bus, q, conn)

    # we call Close...
    call_async(q, chan, 'Close')
    q.expect('dbus-return', method='Close')

    # ...so gabble announces our unavailable presence to the MUC...
    event = q.expect('stream-presence', to=room + '/test')
    elem = event.stanza
    assertEquals('unavailable', elem['type'])

    # ...which the conference server echos...
    echo_muc_presence(q, stream, elem, 'none', 'participant')

    # ...and only now is the channel closed.
    q.expect_many(EventPattern('dbus-signal', signal='Closed'),
                  EventPattern('dbus-signal', signal='ChannelClosed'))

if __name__ == '__main__':
    exec_test(test)
