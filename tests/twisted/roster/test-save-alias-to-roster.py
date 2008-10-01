
"""
Test that updating an alias saves it to the roster.
"""

import dbus

from servicetest import EventPattern, call_async
from gabbletest import acknowledge_iq, exec_test, make_result_iq

def test(q, bus, conn, stream):
    conn.Connect()
    _, event, event2 = q.expect_many(
        EventPattern('dbus-signal', signal='StatusChanged', args=[0, 1]),
        EventPattern('stream-iq', to=None, query_ns='vcard-temp',
            query_name='vCard'),
        EventPattern('stream-iq', query_ns='jabber:iq:roster'))

    acknowledge_iq(stream, event.stanza)
    acknowledge_iq(stream, event2.stanza)

    while True:
        event = q.expect('dbus-signal', signal='NewChannel')
        path, type, handle_type, handle, suppress_handler = event.args

        if type != u'org.freedesktop.Telepathy.Channel.Type.ContactList':
            continue

        chan_name = conn.InspectHandles(handle_type, [handle])[0]

        if chan_name == 'subscribe':
            break

    # request subscription
    chan = bus.get_object(conn.bus_name, path)
    group_iface = dbus.Interface(chan,
        u'org.freedesktop.Telepathy.Channel.Interface.Group')
    assert group_iface.GetMembers() == []
    handle = conn.RequestHandles(1, ['bob@foo.com'])[0]
    group_iface.AddMembers([handle], '')

    event = q.expect('stream-iq', iq_type='set', query_ns='jabber:iq:roster')
    item = event.query.firstChildElement()

    acknowledge_iq(stream, event.stanza)

    call_async(q, conn.Aliasing, 'RequestAliases', [handle])

    event = q.expect('stream-iq', iq_type='get',
        query_ns='http://jabber.org/protocol/pubsub',
        to='bob@foo.com')

    result = make_result_iq(stream, event.stanza)
    pubsub = result.firstChildElement()
    items = pubsub.addElement('items')
    items['node'] = 'http://jabber.org/protocol/nick'
    item = items.addElement('item')
    item.addElement('nick', 'http://jabber.org/protocol/nick',
        content='Bobby')
    stream.send(result)

    event = q.expect('stream-iq', iq_type='set', query_ns='jabber:iq:roster')
    item = event.query.firstChildElement()
    assert item['jid'] == 'bob@foo.com'
    assert item['name'] == 'Bobby'

    q.expect('dbus-return', method='RequestAliases',
        value=(['Bobby'],))

    conn.Disconnect()

    q.expect('dbus-signal', signal='StatusChanged', args=[2, 1])

if __name__ == '__main__':
    exec_test(test)
