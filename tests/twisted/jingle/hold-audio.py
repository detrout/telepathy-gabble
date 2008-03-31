"""
Test the Hold API.
"""

from gabbletest import exec_test, make_result_iq, acknowledge_iq
from servicetest import make_channel_proxy, unwrap, tp_path_prefix, \
        call_async, EventPattern
import jingletest
import gabbletest
import dbus
import time


def test(q, bus, conn, stream):
    jt = jingletest.JingleTest(stream, 'test@localhost', 'foo@bar.com/Foo')

    # If we need to override remote caps, feats, codecs or caps,
    # this is a good time to do it

    # Connecting
    conn.Connect()

    q.expect('dbus-signal', signal='StatusChanged', args=[1, 1])
    q.expect('stream-authenticated')
    q.expect('dbus-signal', signal='PresenceUpdate',
        args=[{1L: (0L, {u'available': {}})}])
    q.expect('dbus-signal', signal='StatusChanged', args=[0, 1])

    # We need remote end's presence for capabilities
    jt.send_remote_presence()

    # Gabble doesn't trust it, so makes a disco
    event = q.expect('stream-iq', query_ns='http://jabber.org/protocol/disco#info',
             to='foo@bar.com/Foo')

    jt.send_remote_disco_reply(event.stanza)

    handle = conn.RequestHandles(1, [jt.remote_jid])[0]

    path = conn.RequestChannel(
        'org.freedesktop.Telepathy.Channel.Type.StreamedMedia',
        1, handle, True)

    signalling_iface = make_channel_proxy(conn, path, 'Channel.Interface.MediaSignalling')
    media_iface = make_channel_proxy(conn, path, 'Channel.Type.StreamedMedia')
    group_iface = make_channel_proxy(conn, path, 'Channel.Interface.Group')
    hold_iface = make_channel_proxy(conn, path, 'Channel.Interface.Hold')

    # S-E gets notified about new session handler, and calls Ready on it
    e = q.expect('dbus-signal', signal='NewSessionHandler')
    assert e.args[1] == 'rtp'

    session_handler = make_channel_proxy(conn, e.args[0], 'Media.SessionHandler')
    session_handler.Ready()

    media_iface.RequestStreams(1, [0]) # 0 == MEDIA_STREAM_TYPE_AUDIO

    e = q.expect('dbus-signal', signal='NewStreamHandler')

    stream_handler = make_channel_proxy(conn, e.args[0], 'Media.StreamHandler')

    stream_handler.NewNativeCandidate("fake", jt.get_remote_transports_dbus())
    stream_handler.Ready(jt.get_audio_codecs_dbus())
    stream_handler.StreamState(2)

    e = q.expect('stream-iq')
    print e.iq_type, e.stanza
    assert e.query.name == 'jingle'
    assert e.query['action'] == 'session-initiate'
    stream.send(gabbletest.make_result_iq(stream, e.stanza))

    jt.outgoing_call_reply(e.query['sid'], True)

    q.expect('stream-iq', iq_type='result')

    # ---- Test 1: GetHoldState returns False and unhold is a no-op ----

    hold_state = hold_iface.GetHoldState()
    assert not hold_state
    hold_iface.RequestHold(False)

    # ---- Test 2: successful hold ----

    call_async(q, hold_iface, 'RequestHold', True)
    e = q.expect('dbus-signal', signal='SetStreamHeld', args=[True])
    call_async(q, stream_handler, 'HoldState', True)

    q.expect_many(
        EventPattern('dbus-return', method='HoldState', value=()),
        EventPattern('dbus-return', method='RequestHold', value=()),
        EventPattern('dbus-signal', signal='HoldStateChanged', args=[True]),
        )

    # ---- Test 3: GetHoldState returns True and hold is a no-op ----

    hold_state = hold_iface.GetHoldState()
    assert hold_state
    hold_iface.RequestHold(True)

    # ---- Test 4: successful unhold ----

    call_async(q, hold_iface, 'RequestHold', False)
    e = q.expect('dbus-signal', signal='SetStreamHeld', args=[False])
    call_async(q, stream_handler, 'HoldState', False)

    q.expect_many(
        EventPattern('dbus-return', method='HoldState', value=()),
        EventPattern('dbus-return', method='RequestHold', value=()),
        EventPattern('dbus-signal', signal='HoldStateChanged', args=[False]),
        )

    # ---- Test 5: GetHoldState returns False and unhold is a no-op ----

    hold_state = hold_iface.GetHoldState()
    assert not hold_state
    hold_iface.RequestHold(False)

    # ---- Test 6: 3 parallel calls to hold ----

    call_async(q, hold_iface, 'RequestHold', True)
    call_async(q, hold_iface, 'RequestHold', True)
    call_async(q, hold_iface, 'RequestHold', True)

    e = q.expect('dbus-signal', signal='SetStreamHeld', args=[True])

    call_async(q, stream_handler, 'HoldState', True)

    # FIXME: this hard-codes the order in which things happen - enhance
    # expect_many to be able to require multiple similar events to happen
    q.expect('dbus-signal', signal='HoldStateChanged', args=[True])
    q.expect('dbus-return', method='RequestHold', value=())
    q.expect('dbus-return', method='RequestHold', value=())
    q.expect('dbus-return', method='RequestHold', value=())
    q.expect('dbus-return', method='HoldState', value=())

    # ---- Test 7: 3 parallel calls to unhold ----

    hold_state = hold_iface.GetHoldState()
    assert hold_state

    call_async(q, hold_iface, 'RequestHold', False)
    call_async(q, hold_iface, 'RequestHold', False)
    call_async(q, hold_iface, 'RequestHold', False)

    e = q.expect('dbus-signal', signal='SetStreamHeld', args=[False])

    call_async(q, stream_handler, 'HoldState', False)

    # FIXME: this hard-codes the order in which things happen - enhance
    # expect_many to be able to require multiple similar events to happen
    q.expect('dbus-signal', signal='HoldStateChanged', args=[False])
    q.expect('dbus-return', method='RequestHold', value=())
    q.expect('dbus-return', method='RequestHold', value=())
    q.expect('dbus-return', method='RequestHold', value=())
    q.expect('dbus-return', method='HoldState', value=())

    # ---- Test 8: hold, then change our minds before s-e has responded ----

    hold_state = hold_iface.GetHoldState()
    assert not hold_state

    call_async(q, hold_iface, 'RequestHold', True)
    call_async(q, hold_iface, 'RequestHold', False)
    e = q.expect('dbus-signal', signal='SetStreamHeld', args=[True])
    e = q.expect('dbus-error', method='RequestHold')
    assert str(e.error) == \
            ('org.freedesktop.Telepathy.Errors.NotAvailable: '
             'RequestHold(TRUE) cancelled by RequestHold(FALSE)'), e.error
    e = q.expect('dbus-signal', signal='SetStreamHeld', args=[False])

    call_async(q, stream_handler, 'HoldState', True)
    call_async(q, stream_handler, 'HoldState', False)

    q.expect_many(
        EventPattern('dbus-return', method='HoldState', value=()),
        EventPattern('dbus-return', method='RequestHold', value=()),
        EventPattern('dbus-signal', signal='HoldStateChanged', args=[False]),
        )

    # ---- Test 9: unhold, then change our minds before s-e has responded ----

    # (we need to hold first)
    call_async(q, hold_iface, 'RequestHold', True)
    e = q.expect('dbus-signal', signal='SetStreamHeld', args=[True])
    call_async(q, stream_handler, 'HoldState', True)
    q.expect_many(
        EventPattern('dbus-return', method='HoldState', value=()),
        EventPattern('dbus-return', method='RequestHold', value=()),
        EventPattern('dbus-signal', signal='HoldStateChanged', args=[True]),
        )

    # OK, now do the unhold/hold cycle
    call_async(q, hold_iface, 'RequestHold', False)
    call_async(q, hold_iface, 'RequestHold', True)
    e = q.expect('dbus-signal', signal='SetStreamHeld', args=[False])
    e = q.expect('dbus-error', method='RequestHold')
    assert str(e.error) == \
            ('org.freedesktop.Telepathy.Errors.NotAvailable: '
             'RequestHold(FALSE) cancelled by RequestHold(TRUE)'), e.error
    e = q.expect('dbus-signal', signal='SetStreamHeld', args=[True])

    call_async(q, stream_handler, 'HoldState', False)
    call_async(q, stream_handler, 'HoldState', True)

    q.expect_many(
        EventPattern('dbus-return', method='HoldState', value=()),
        EventPattern('dbus-return', method='RequestHold', value=()),
        EventPattern('dbus-signal', signal='HoldStateChanged', args=[True]),
        )

    # Unhold, just to get back to a "normal" state
    call_async(q, hold_iface, 'RequestHold', False)
    e = q.expect('dbus-signal', signal='SetStreamHeld', args=[False])
    call_async(q, stream_handler, 'HoldState', False)

    q.expect_many(
        EventPattern('dbus-return', method='HoldState', value=()),
        EventPattern('dbus-return', method='RequestHold', value=()),
        EventPattern('dbus-signal', signal='HoldStateChanged', args=[False]),
        )

    # Time passes ... afterwards we close the chan

    group_iface.RemoveMembers([dbus.UInt32(1)], 'closed')

    # Test completed, close the connection

    e = q.expect('dbus-signal', signal='Close') #XXX - match against the path

    conn.Disconnect()
    q.expect('dbus-signal', signal='StatusChanged', args=[2, 1])

    return True


if __name__ == '__main__':
    exec_test(test)
