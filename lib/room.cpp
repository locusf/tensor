/******************************************************************************
 * Copyright (C) 2015 Felix Rohrbach <kde@fxrh.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "room.h"

#include <QLinkedList>

#include <QtCore/QJsonArray>
#include <QtCore/QDebug>
#include <QtCore/QStringBuilder> // for efficient string concats (operator%)

#include "connection.h"
#include "state.h"
#include "user.h"
#include "events/event.h"
#include "events/roommessageevent.h"
#include "events/roomnameevent.h"
#include "events/roomaliasesevent.h"
#include "events/roomcanonicalaliasevent.h"
#include "events/roomtopicevent.h"
#include "events/roommemberevent.h"
#include "events/typingevent.h"
#include "events/receiptevent.h"
#include "jobs/roommessagesjob.h"

using namespace QMatrixClient;

class Room::Private
{
    public:
        Private(Room* parent): q(parent) {}

        Room* q;

        //static LogMessage* parseMessage(const QJsonObject& message);
        void renameUser(User* user, QString oldName);
        void updateDisplayname();

        Connection* connection;
        events_list_t messageEvents;
        QString id;
        QStringList aliases;
        QString canonicalAlias;
        QString name;
        QString displayname;
        QString topic;
        JoinState joinState;
        int highlightCount;
        int notificationCount;
        members_list_t users;
        QList<User*> usersTyping;
        QList<User*> usersLeft;
        QHash<User*, QString> lastReadEvent;
        QString prevBatch;
        bool gettingNewContent;

    private:
        QString roomNameFromMemberNames(const QList<User *> & userlist);
};

Room::Room(Connection* connection, QString id)
    : d(new Private(this))
{
    d->id = id;
    d->connection = connection;
    d->joinState = JoinState::Join;
    d->displayname = "Empty room <" % id % ">";
    d->gettingNewContent = false;
    qDebug() << "New Room:" << id;

    //connection->getMembers(this); // I don't think we need this anymore in r0.0.1
}

Room::~Room()
{
    delete d;
}

QString Room::id() const
{
    return d->id;
}

Room::events_list_t Room::messages() const
{
    return d->messageEvents;
}

QString Room::name() const
{
    return d->name;
}

QStringList Room::aliases() const
{
    return d->aliases;
}

QString Room::canonicalAlias() const
{
    return d->canonicalAlias;
}

QString Room::displayName() const
{
    return d->displayname;
}

QString Room::topic() const
{
    return d->topic;
}

JoinState Room::joinState() const
{
    return d->joinState;
}

void Room::setJoinState(JoinState state)
{
    JoinState oldState = d->joinState;
    if( state == oldState )
        return;
    d->joinState = state;
    emit joinStateChanged(oldState, state);
}

void Room::markMessageAsRead(Event* event)
{
    d->connection->postReceipt(this, event);
}

QString Room::lastReadEvent(User* user)
{
    return d->lastReadEvent.value(user);
}

int Room::notificationCount() const
{
    return d->notificationCount;
}

void Room::resetNotificationCount()
{
    if( d->notificationCount == 0 )
        return;
    d->notificationCount = 0;
    emit notificationCountChanged(this);
}

int Room::highlightCount() const
{
    return d->highlightCount;
}

void Room::resetHighlightCount()
{
if( d->highlightCount == 0 )
        return;
    d->highlightCount = 0;
    emit highlightCountChanged(this);
}

QList< User* > Room::usersTyping() const
{
    return d->usersTyping;
}

QList<User *> Room::usersLeft() const
{
   return d->usersLeft;
}

Room::members_list_t Room::users() const
{
    return d->users;
}

QString Room::roomMembername(User *u) const
{
    // See the CS spec, section 11.2.2.3

    QString username = u->name();
    if (username.isEmpty())
        return u->id();

    auto namesakes = d->users.values(username);
    if (namesakes.size() == 1)
        return username;

#ifndef NDEBUG
    // This is just a sanity check of our own data structures
    auto u_it = std::find(namesakes.begin(), namesakes.end(), u);
    if ( /* unlikely */ u_it == namesakes.end())
    {
        // Also treats the (also suspicious) case of namesakes.empty()
        qDebug() << "Room::uniqueDisplayname(): no user" << u->id()
                 << "in the room" << displayName();
        return username;
    }
#endif

    return username % " <" % u->id() % ">";
}

void Room::addMessage(Event* event)
{
    processMessageEvent(event);
    emit newMessage(event);
    //d->addState(event);
}

void Room::addInitialState(State* state)
{
    processStateEvent(state->event());
}

void Room::updateData(const SyncRoomData& data)
{
    if( d->prevBatch.isEmpty() )
        d->prevBatch = data.timelinePrevBatch;
    setJoinState(data.joinState);

    for( Event* stateEvent: data.state )
    {
        processStateEvent(stateEvent);
    }

    for( Event* timelineEvent: data.timeline )
    {

        processMessageEvent(timelineEvent);
        emit newMessage(timelineEvent);
        // State changes can arrive in a timeline event - try to check those.
        processStateEvent(timelineEvent);
    }

    for( Event* ephemeralEvent: data.ephemeral )
    {
        processEphemeralEvent(ephemeralEvent);
    }

    if( data.highlightCount != d->highlightCount )
    {
        d->highlightCount = data.highlightCount;
        emit highlightCountChanged(this);
    }
    if( data.notificationCount != d->notificationCount )
    {
        d->notificationCount = data.notificationCount;
        emit notificationCountChanged(this);
    }
}

void Room::getPreviousContent()
{
    if( !d->gettingNewContent )
    {
        d->gettingNewContent = true;
        RoomMessagesJob* job = d->connection->getMessages(this, d->prevBatch);
        connect( job, &RoomMessagesJob::result, this, &Room::gotMessages );
    }
}

void Room::userRenamed(User *user, QString oldName)
{
    d->renameUser(user, oldName);
}

void Room::gotMessages(KJob* job)
{
    RoomMessagesJob* realJob = static_cast<RoomMessagesJob*>(job);
    if( realJob->error() )
    {
        qDebug() << realJob->errorString();
    }
    else
        {
        for( Event* event: realJob->events() )
        {
            processMessageEvent(event);
            emit newMessage(event);
        }
        d->prevBatch = realJob->end();
    }
    d->gettingNewContent = false;
}

Connection* Room::connection()
{
    return d->connection;
}

void Room::processMessageEvent(Event* event)
{
    d->messageEvents.insert(event->findEarliestAfterMe(d->messageEvents), event);
}

void Room::processStateEvent(Event* event)
{
    if (!event)
    {
        qDebug() << "!!! nullptr passed to Room::Private::addState";
        return;
    }

    if( event->type() == EventType::RoomName )
    {
        RoomNameEvent* nameEvent = static_cast<RoomNameEvent*>(event);
        d->name = nameEvent->name();
        qDebug() << "room name:" << d->name;
        d->updateDisplayname();
    }
    if( event->type() == EventType::RoomAliases )
    {
        RoomAliasesEvent* aliasesEvent = static_cast<RoomAliasesEvent*>(event);
        d->aliases = aliasesEvent->aliases();
        qDebug() << "room aliases:" << d->aliases;
        d->updateDisplayname();
    }
    if( event->type() == EventType::RoomCanonicalAlias )
    {
        RoomCanonicalAliasEvent* aliasEvent = static_cast<RoomCanonicalAliasEvent*>(event);
        d->canonicalAlias = aliasEvent->alias();
        qDebug() << "room canonical alias:" << d->canonicalAlias;
        d->updateDisplayname();
    }
    if( event->type() == EventType::RoomTopic )
    {
        RoomTopicEvent* topicEvent = static_cast<RoomTopicEvent*>(event);
        d->topic = topicEvent->topic();
        emit topicChanged();
    }
    if( event->type() == EventType::RoomMember )
    {
        RoomMemberEvent* memberEvent = static_cast<RoomMemberEvent*>(event);
        User* u = d->connection->user(memberEvent->userId());

        u->processEvent(memberEvent);
        if( memberEvent->membership() == MembershipType::Join
            && !d->users.values(u->name()).contains(u) )
        {
            d->users.insert(u->name(), u);
            connect(u, &User::nameChanged, this, &Room::userRenamed);
            emit userAdded(u);
            d->updateDisplayname();
        }
        else if( memberEvent->membership() == MembershipType::Leave )
        {
            if( d->users.values(u->name()).contains(u) )
                d->users.remove(u->name(), u);
            if( !d->usersLeft.contains(u) ) // which is strange
                d->usersLeft.append(u);
            disconnect(u, &User::nameChanged, this, &Room::userRenamed);
            emit userRemoved(u);
            d->updateDisplayname();
        }
    }
}

void Room::processEphemeralEvent(Event* event)
{
    if( event->type() == EventType::Typing )
    {
        TypingEvent* typingEvent = static_cast<TypingEvent*>(event);
        d->usersTyping.clear();
        for( const QString& userid: typingEvent->users() )
        {
            d->usersTyping.append(d->connection->user(userid));
        }
        emit typingChanged();
    }
    if( event->type() == EventType::Receipt )
    {
        ReceiptEvent* receiptEvent = static_cast<ReceiptEvent*>(event);
        for( QString eventId: receiptEvent->events() )
        {
            QList<Receipt> receipts = receiptEvent->receiptsForEvent(eventId);
            for( Receipt r: receipts )
            {
                d->lastReadEvent.insert(d->connection->user(r.userId), eventId);
            }
        }
    }
}

void Room::Private::renameUser(User *user, QString oldName)
{
    if (users.values(oldName).contains(user))
    {
        // Re-add the user to the hashmap under a new name.
        users.remove(oldName, user);
        users.insert(user->name(), user);
        updateDisplayname();
    }
}

QString Room::Private::roomNameFromMemberNames(const QList<User *> &userlist)
{
    QList<User *> first_two{nullptr,nullptr};
    std::partial_sort_copy(
        userlist.begin(), userlist.end(),
        first_two.begin(), first_two.end(),
        [this](const User* u1, const User* u2) {
            // Filter out the "me" user so that it never hits the room name
            return u1 != connection->user() && u1->id() < u2->id();
        }
    );

    if (userlist.size() == 2)
        return q->roomMembername(first_two.at(0));

    if (userlist.size() == 3)
        return q->roomMembername(first_two.at(0)) %
                " and " % q->roomMembername(first_two.at(1));

    if (userlist.size() > 3)
        return QString("%1 and %2 others")
                .arg(q->roomMembername(first_two.at(0)))
                .arg(userlist.size() - 3); // To make it locale-aware, use %L2

    return QString();
}

void Room::Private::updateDisplayname()
{
    const QString old_name = displayname;

    // CS spec, section 11.2.2.5 Calculating the display name for a room
    // Numbers and i's below refer to respective parts in the spec.
    do {
        // while (false) - we'll break out of the sequence once the name is
        // ready inside if statements

        // 1. Name (from m.room.name)
        if (!name.isEmpty()) {
            // The below is spec extension.
            // If we have a non-empty m.room.name, try to stack a canonical alias to it.
            // The format is unwittingly borrowed from the email address format.
            displayname = name;
            if (!canonicalAlias.isEmpty())
                displayname += " <" % canonicalAlias % ">";
            break;
        }

        // 2. Canonical alias
        if (!canonicalAlias.isEmpty()) {
            displayname = canonicalAlias;
            break;
        }

        // 3. Room members
        // The spec requires to sort users lexicographically by state_key
        // (i.e. user id) and use disambiguated display names of two topmost
        // users for the name of the room. Ok, let's do it.
        displayname = roomNameFromMemberNames(users.values());
        if (!displayname.isEmpty())
            break;

        // 4. Users that previously left the room
        displayname = roomNameFromMemberNames(usersLeft);
        if (!displayname.isEmpty())
            break;

        // 5. Fail miserably
        displayname = "Empty room (" % id % ")";

        // Using m.room.aliases is explicitly discouraged by the spec
        //if (!aliases().empty() && !aliases().at(0).isEmpty())
        //    displayname = aliases().at(0);
    } while (false);

    if (old_name != displayname)
        emit q->namesChanged(q);
}

// void Room::setAlias(QString alias)
// {
//     d->alias = alias;
//     emit aliasChanged(this);
// }
//
// bool Room::parseEvents(const QJsonObject& json)
// {
//     QList<LogMessage*> newMessages;
//     QJsonValue value = json.value("messages").toObject().value("chunk");
//     if( !value.isArray() )
//     {
//         return false;
//     }
//     QJsonArray messages = value.toArray();
//     for(const QJsonValue& val: messages )
//     {
//         if( !val.isObject() )
//             continue;
//         LogMessage* msg = Private::parseMessage(val.toObject());
//         if( msg )
//         {
//             newMessages.append(msg);
//         }
//
//     }
//     addMessages(newMessages);
//     return true;
// }
//
// bool Room::parseSingleEvent(const QJsonObject& json)
// {
//     qDebug() << "parseSingleEvent";
//     LogMessage* msg = Private::parseMessage(json);
//     if( msg )
//     {
//         addMessage(msg);
//         return true;
//     }
//     return false;
// }
//
// bool Room::parseState(const QJsonObject& json)
// {
//     QJsonValue value = json.value("state");
//     if( !value.isArray() )
//     {
//         return false;
//     }
//     QJsonArray states = value.toArray();
//     for( const QJsonValue& val: states )
//     {
//         QJsonObject state = val.toObject();
//         QString type = state.value("type").toString();
//         if( type == "m.room.aliases" )
//         {
//             QJsonArray aliases = state.value("content").toObject().value("aliases").toArray();
//             if( aliases.count() > 0 )
//             {
//                 setAlias(aliases.at(0).toString());
//             }
//         }
//     }
//     return true;
// }
//
// LogMessage* Room::Private::parseMessage(const QJsonObject& message)
// {
//     if( message.value("type") == "m.room.message" )
//     {
//         QJsonObject content = message.value("content").toObject();
//         if( content.value("msgtype").toString() != "m.text" )
//             return 0;
//         QString user = message.value("user_id").toString();
//         QString body = content.value("body").toString();
//         LogMessage* msg = new LogMessage( LogMessage::UserMessage, body, user );
//         return msg;
//     }
//     return 0;
// }

