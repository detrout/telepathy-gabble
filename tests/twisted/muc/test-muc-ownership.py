
"""
Test support for the HANDLE_OWNERS_NOT_AVAILABLE group flag, and calling
HandleOwners on MUC members.

By default, MUC channels should have the flag set. The flag should be unset
when presence is received that includes the MUC JID's owner JID.
"""

from gabbletest import exec_test, make_muc_presence
from servicetest import (
    call_async, EventPattern, assertEquals, assertFlagsSet, assertFlagsUnset,
    wrap_channel,
    )
import constants as cs

def test(q, bus, conn, stream):
    self_handle = conn.Properties.Get(cs.CONN, "SelfHandle")

    call_async(q, conn.Requests, 'CreateChannel',
            { cs.CHANNEL_TYPE: cs.CHANNEL_TYPE_TEXT,
              cs.TARGET_HANDLE_TYPE: cs.HT_ROOM,
              cs.TARGET_ID: 'chat@conf.localhost'
            })

    gfc, _, _, _ = q.expect_many(
        # Initial group flags
        EventPattern('dbus-signal', signal='GroupFlagsChanged',
            predicate=lambda e: e.args[0] != 0),
        EventPattern('dbus-signal', signal='MembersChangedDetailed',
            predicate=lambda e: e.args[3] == [2]),
        # Removing CAN_ADD
        EventPattern('dbus-signal', signal='GroupFlagsChanged',
          args = [0, cs.GF_CAN_ADD], predicate=lambda e: e.args[0] == 0),
        EventPattern('stream-presence', to='chat@conf.localhost/test'))
    assert gfc.args[1] == 0

    # Send presence for anonymous other member of room.
    stream.send(make_muc_presence('owner', 'moderator', 'chat@conf.localhost', 'bob'))

    # Send presence for anonymous other member of room (2)
    stream.send(make_muc_presence('owner', 'moderator', 'chat@conf.localhost', 'brian'))

    # Send presence for nonymous other member of room.
    stream.send(make_muc_presence('none', 'participant', 'chat@conf.localhost',
        'che', 'che@foo.com'))

    # Send presence for nonymous other member of room (2)
    stream.send(make_muc_presence('none', 'participant', 'chat@conf.localhost',
        'chris', 'chris@foo.com'))

    # Send presence for own membership of room.
    stream.send(make_muc_presence('none', 'participant', 'chat@conf.localhost', 'test'))

    # Since we received MUC presence that contains an owner JID, the
    # OWNERS_NOT_AVAILABLE flag should be removed.
    event = q.expect('dbus-signal', signal='GroupFlagsChanged',
        args = [0, cs.GF_HANDLE_OWNERS_NOT_AVAILABLE ])

    event = q.expect('dbus-signal', signal='HandleOwnersChanged')
    owners = event.args[0]

    event = q.expect('dbus-signal', signal='MembersChangedDetailed')
    added = event.args[0]

    [test, bob, brian, che, che_owner, chris, chris_owner] = \
        conn.get_contact_handles_sync(
            [ 'chat@conf.localhost/test', 'chat@conf.localhost/bob',
              'chat@conf.localhost/brian', 'chat@conf.localhost/che',
              'che@foo.com', 'chat@conf.localhost/chris', 'chris@foo.com',
            ])
    expected_members = sorted([test, bob, brian, che, chris])
    expected_owners = { test: self_handle,
                        bob: 0,
                        brian: 0,
                        che: che_owner,
                        chris: chris_owner
                      }
    assertEquals(expected_members, sorted(added))
    assertEquals(expected_owners, owners)

    event = q.expect('dbus-return', method='CreateChannel')

    chan = wrap_channel(bus.get_object(conn.bus_name, event.value[0]), 'Text')

    # Exercise HandleOwners
    owners = chan.Properties.Get(cs.CHANNEL_IFACE_GROUP, 'HandleOwners')
    assertEquals(che_owner, owners[che])
    assertEquals(chris_owner, owners[chris])

    # Exercise D-Bus properties
    all = chan.Properties.GetAll(cs.CHANNEL_IFACE_GROUP)

    assert all[u'LocalPendingMembers'] == [], all
    assert sorted(all[u'Members']) == expected_members, all
    assert all[u'RemotePendingMembers'] == [], all
    assert all[u'SelfHandle'] == test, all
    assert all[u'HandleOwners'] == expected_owners, all

    flags = all[u'GroupFlags']
    assertFlagsSet(cs.GF_CHANNEL_SPECIFIC_HANDLES, flags)
    assertFlagsUnset(cs.GF_HANDLE_OWNERS_NOT_AVAILABLE, flags)

if __name__ == '__main__':
    exec_test(test)
