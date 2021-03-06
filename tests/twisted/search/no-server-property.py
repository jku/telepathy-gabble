"""
Tests Contact Search channels to a simulated XEP-0055 service, without
passing the Server property
"""

import dbus

from twisted.words.xish import xpath
from functools import partial

from gabbletest import exec_test, sync_stream, make_result_iq, acknowledge_iq, elem_iq, elem, disconnect_conn
from servicetest import EventPattern, assertEquals
from search_helper import call_create, answer_field_query

import constants as cs
import ns

JUD_SERVER = 'jud.localhost'

def server_discovered(q, bus, conn, stream, server=None):
    iq_event, disco_event = q.expect_many(
        EventPattern('stream-iq', to=None, query_ns='vcard-temp',
            query_name='vCard'),
        EventPattern('stream-iq', to='localhost', query_ns=ns.DISCO_ITEMS))

    acknowledge_iq(stream, iq_event.stanza)

    # no search server has been discovered yet. The CreateChannel operation
    # will be completed once the disco process is finished.
    call_create(q, conn, server=server)

    # reply to IQ query
    reply = make_result_iq(stream, disco_event.stanza)
    query = xpath.queryForNodes('/iq/query', reply)[0]
    item = query.addElement((None, 'item'))
    item['jid'] = JUD_SERVER
    stream.send(reply)

    # wait for the disco#info query
    event = q.expect('stream-iq', to=JUD_SERVER, query_ns=ns.DISCO_INFO)

    reply = elem_iq(stream, 'result', id=event.stanza['id'], from_=JUD_SERVER)(
        elem(ns.DISCO_INFO, 'query')(
            elem('identity', category='directory', type='user', name='vCard User Search')(),
            elem('feature', var=ns.SEARCH)()))

    stream.send(reply)

    # JUD_SERVER is used as default, and shows up as the Server property on the
    # resulting channel.
    ret, _ = answer_field_query(q, stream, JUD_SERVER)
    _, properties = ret.value
    assertEquals(JUD_SERVER, properties[cs.CONTACT_SEARCH_SERVER])

    # Now that the search server has been discovered, it is used right away.
    call_create(q, conn, server=server)
    ret, _ = answer_field_query(q, stream, JUD_SERVER)
    _, properties = ret.value
    assertEquals(JUD_SERVER, properties[cs.CONTACT_SEARCH_SERVER])

def no_server_discovered(q, bus, conn, stream, server=None):
    iq_event, disco_event = q.expect_many(
        EventPattern('stream-iq', to=None, query_ns='vcard-temp',
            query_name='vCard'),
        EventPattern('stream-iq', to='localhost', query_ns=ns.DISCO_ITEMS))

    acknowledge_iq(stream, iq_event.stanza)

    # no search server has been discovered yet. The CreateChannel operation
    # will fail once the disco process is finished.
    call_create(q, conn, server=server)

    # reply to IQ query. No search server is present
    reply = make_result_iq(stream, disco_event.stanza)
    stream.send(reply)

    # creation of the channel failed
    e = q.expect('dbus-error', method='CreateChannel', name=cs.INVALID_ARGUMENT)

    # This server doesn't have a search server. We can't create Search channel
    # without specifying a Server property
    call_create(q, conn, server=server)
    e = q.expect('dbus-error', method='CreateChannel')
    assertEquals(cs.INVALID_ARGUMENT, e.error.get_dbus_name())

def disconnect_before_disco(q, bus, conn, stream, server=None):
    iq_event, disco_event = q.expect_many(
        EventPattern('stream-iq', to=None, query_ns='vcard-temp',
            query_name='vCard'),
        EventPattern('stream-iq', to='localhost', query_ns=ns.DISCO_ITEMS))

    acknowledge_iq(stream, iq_event.stanza)

    # try to create a channel before the disco process is completed.
    # This creation will fail
    call_create(q, conn, server=server)

    # connection is disconnected. CreateChannel fails
    disconnect_conn(q, conn, stream, [
        EventPattern('dbus-error', method='CreateChannel', name=cs.DISCONNECTED)])

if __name__ == '__main__':
    exec_test(server_discovered)
    exec_test(no_server_discovered)
    exec_test(disconnect_before_disco)
    # We also test that Gabble does something sensible if you set Server to the
    # empty string, rather than omitting it.
    exec_test(partial(server_discovered, server=''))
    exec_test(partial(no_server_discovered, server=''))
    exec_test(partial(disconnect_before_disco, server=''))
