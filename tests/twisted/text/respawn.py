"""
Test text channel being recreated because there are still pending messages.
"""

import dbus

from twisted.words.xish import domish

from gabbletest import exec_test
from servicetest import call_async, EventPattern, assertEquals
import constants as cs

def test(q, bus, conn, stream):
    self_handle = conn.GetSelfHandle()

    jid = 'foo@bar.com'
    foo_handle = conn.RequestHandles(cs.HT_CONTACT, [jid])[0]

    call_async(q, conn, 'RequestChannel',
        cs.CHANNEL_TYPE_TEXT, cs.HT_CONTACT, foo_handle, True)

    ret, old_sig, new_sig = q.expect_many(
        EventPattern('dbus-return', method='RequestChannel'),
        EventPattern('dbus-signal', signal='NewChannel'),
        EventPattern('dbus-signal', signal='NewChannels'),
        )

    text_chan = bus.get_object(conn.bus_name, ret.value[0])
    chan_iface = dbus.Interface(text_chan, cs.CHANNEL)
    text_iface = dbus.Interface(text_chan, cs.CHANNEL_TYPE_TEXT)

    assert old_sig.args[0] == ret.value[0]
    assert old_sig.args[1] == cs.CHANNEL_TYPE_TEXT
    assert old_sig.args[2] == cs.HT_CONTACT
    assert old_sig.args[3] == foo_handle
    assert old_sig.args[4] == True      # suppress handler

    assert len(new_sig.args) == 1
    assert len(new_sig.args[0]) == 1        # one channel
    assert len(new_sig.args[0][0]) == 2     # two struct members
    assert new_sig.args[0][0][0] == ret.value[0]
    emitted_props = new_sig.args[0][0][1]
    assert emitted_props[cs.CHANNEL_TYPE] == cs.CHANNEL_TYPE_TEXT
    assert emitted_props[cs.TARGET_HANDLE_TYPE] == cs.HT_CONTACT
    assert emitted_props[cs.TARGET_HANDLE] == foo_handle
    assert emitted_props[cs.TARGET_ID] == jid
    assert emitted_props[cs.REQUESTED] == True
    assert emitted_props[cs.INITIATOR_HANDLE] == self_handle
    assert emitted_props[cs.INITIATOR_ID] == 'test@localhost'

    channel_props = text_chan.GetAll(
        cs.CHANNEL, dbus_interface=dbus.PROPERTIES_IFACE)
    assert channel_props['TargetID'] == jid,\
            (channel_props['TargetID'], jid)
    assert channel_props['Requested'] == True
    assert channel_props['InitiatorHandle'] == self_handle,\
            (channel_props['InitiatorHandle'], self_handle)
    assert channel_props['InitiatorID'] == 'test@localhost',\
            channel_props['InitiatorID']

    text_iface.Send(0, 'hey')

    event = q.expect('stream-message')

    elem = event.stanza
    assert elem.name == 'message'
    assert elem['type'] == 'chat'
    body = list(event.stanza.elements())[0]
    assert body.name == 'body'
    assert body.children[0] == u'hey'

    # <message type="chat"><body>hello</body</message>
    m = domish.Element((None, 'message'))
    m['from'] = 'foo@bar.com/Pidgin'
    m['type'] = 'chat'
    m.addElement('body', content='hello')
    stream.send(m)

    event = q.expect('dbus-signal', signal='Received')

    hello_message_id = event.args[0]
    hello_message_time = event.args[1]
    assert event.args[2] == foo_handle
    # message type: normal
    assert event.args[3] == 0
    # flags: none
    assert event.args[4] == 0
    # body
    assert event.args[5] == 'hello'

    messages = text_chan.ListPendingMessages(False,
        dbus_interface=cs.CHANNEL_TYPE_TEXT)
    assert messages == \
            [(hello_message_id, hello_message_time, foo_handle,
                0, 0, 'hello')], messages

    # close the channel without acking the message; it comes back

    call_async(q, chan_iface, 'Close')

    old, new = q.expect_many(
            EventPattern('dbus-signal', signal='Closed'),
            EventPattern('dbus-signal', signal='ChannelClosed'),
            )
    assert old.path == text_chan.object_path,\
            (old.path, text_chan.object_path)
    assert new.args[0] == text_chan.object_path,\
            (new.args[0], text_chan.object_path)

    event = q.expect('dbus-signal', signal='NewChannel')
    assert event.args[0] == text_chan.object_path
    assert event.args[1] == cs.CHANNEL_TYPE_TEXT
    assert event.args[2] == cs.HT_CONTACT
    assert event.args[3] == foo_handle
    assert event.args[4] == False   # suppress handler

    event = q.expect('dbus-return', method='Close')

    # it now behaves as if the message had initiated it

    channel_props = text_chan.GetAll(
        cs.CHANNEL, dbus_interface=dbus.PROPERTIES_IFACE)
    assert channel_props['TargetID'] == jid,\
            (channel_props['TargetID'], jid)
    assert channel_props['Requested'] == False
    assert channel_props['InitiatorHandle'] == foo_handle,\
            (channel_props['InitiatorHandle'], foo_handle)
    assert channel_props['InitiatorID'] == 'foo@bar.com',\
            channel_props['InitiatorID']

    # the message is still there

    messages = text_chan.ListPendingMessages(False,
        dbus_interface=cs.CHANNEL_TYPE_TEXT)
    assertEquals(
        [(hello_message_id, hello_message_time, foo_handle, 0, 8, 'hello')],
        messages)

    # acknowledge it

    text_chan.AcknowledgePendingMessages([hello_message_id],
        dbus_interface=cs.CHANNEL_TYPE_TEXT)

    messages = text_chan.ListPendingMessages(False,
        dbus_interface=cs.CHANNEL_TYPE_TEXT)
    assert messages == []

    # close the channel again

    call_async(q, chan_iface, 'Close')

    event = q.expect('dbus-signal', signal='Closed')
    assert event.path == text_chan.object_path,\
            (event.path, text_chan.object_path)

    event = q.expect('dbus-return', method='Close')

    # assert that it stays dead this time!

    try:
        chan_iface.GetChannelType()
    except dbus.DBusException:
        pass
    else:
        raise AssertionError("Why won't it die?")

if __name__ == '__main__':
    exec_test(test)
