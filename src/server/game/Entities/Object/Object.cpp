/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Common.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "World.h"
#include "Object.h"
#include "Creature.h"
#include "Player.h"
#include "Vehicle.h"
#include "ObjectMgr.h"
#include "UpdateData.h"
#include "UpdateMask.h"
#include "Util.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "Log.h"
#include "Transport.h"
#include "TargetedMovementGenerator.h"
#include "WaypointMovementGenerator.h"
#include "VMapFactory.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "SpellAuraEffects.h"
#include "UpdateFieldFlags.h"
#include "TemporarySummon.h"
#include "Totem.h"
#include "OutdoorPvPMgr.h"
#include "MovementPacketBuilder.h"
#include "DynamicTree.h"
#include "Unit.h"
#include "Group.h"
#include "Battlefield.h"
#include "BattlefieldMgr.h"
#include "ObjectVisitors.hpp"
#include "Map.h"

uint32 GuidHigh2TypeId(uint32 guid_hi)
{
    switch (guid_hi)
    {
        case HIGHGUID_ITEM:         return TYPEID_ITEM;
        //case HIGHGUID_CONTAINER:    return TYPEID_CONTAINER; HIGHGUID_CONTAINER == HIGHGUID_ITEM currently
        case HIGHGUID_UNIT:         return TYPEID_UNIT;
        case HIGHGUID_PET:          return TYPEID_UNIT;
        case HIGHGUID_PLAYER:       return TYPEID_PLAYER;
        case HIGHGUID_GAMEOBJECT:   return TYPEID_GAMEOBJECT;
        case HIGHGUID_DYNAMICOBJECT:return TYPEID_DYNAMICOBJECT;
        case HIGHGUID_CORPSE:       return TYPEID_CORPSE;
        case HIGHGUID_AREATRIGGER:  return TYPEID_AREATRIGGER;
        case HIGHGUID_MO_TRANSPORT: return TYPEID_GAMEOBJECT;
        case HIGHGUID_VEHICLE:      return TYPEID_UNIT;
    }
    return NUM_CLIENT_OBJECT_TYPES;                         // unknown
}

char const* Object::GetTypeName(uint32 high)
{
    switch(high)
    {
        case HIGHGUID_ITEM:         return "Item";
        case HIGHGUID_PLAYER:       return "Player";
        case HIGHGUID_GAMEOBJECT:   return "Gameobject";
        case HIGHGUID_TRANSPORT:    return "Transport";
        case HIGHGUID_UNIT:         return "Creature";
        case HIGHGUID_PET:          return "Pet";
        case HIGHGUID_VEHICLE:      return "Vehicle";
        case HIGHGUID_DYNAMICOBJECT:return "DynObject";
        case HIGHGUID_CORPSE:       return "Corpse";
        case HIGHGUID_MO_TRANSPORT: return "MoTransport";
        default:
            return "<unknown>";
    }
}

std::string Object::GetString() const
{
    if(!m_uint32Values)
        return "NONE";

    std::ostringstream str;
    str << GetTypeName();
    
    if (GetTypeId() == TYPEID_PLAYER)
    {
        std::string name;
        if (ObjectMgr::GetPlayerNameByGUID(GetGUID(), name))
            str << " " << name;
    }

    str << " (";
    if (GetTypeId() == TYPEID_UNIT)
        str << "Entry: " << GetEntry() << " ";
    str << "Guid: " << GetGUIDLow() << ")";
    return str.str();
}

Object::Object() : m_PackGUID(sizeof(uint64)+1), 
    m_objectTypeId(TYPEID_OBJECT), m_objectType(TYPEMASK_OBJECT), m_uint32Values(NULL),
    _changedFields(NULL), m_valuesCount(0), _fieldNotifyFlags(UF_FLAG_NONE), m_inWorld(0),
    m_objectUpdated(false)
{
    m_PackGUID.appendPackGUID(0);
}

WorldObject::~WorldObject()
{
    // this may happen because there are many !create/delete
    if (IsWorldObject() && m_currMap)
    {
        if (GetTypeId() == TYPEID_CORPSE)
        {
            TC_LOG_FATAL("server", "Object::~Object Corpse guid=" UI64FMTD ", type=%d, entry=%u deleted but still in map!!", GetGUID(), ((Corpse*)this)->GetType(), GetEntry());
            ASSERT(false);
        }
        ResetMap();
    }
}

Object::~Object()
{
    if (IsInWorld())
    {
        TC_LOG_FATAL("server", "Object::~Object - guid=" UI64FMTD ", typeid=%d, entry=%u deleted but still in world!!", GetGUID(), GetTypeId(), GetEntry());
        if (isType(TYPEMASK_ITEM))
            TC_LOG_FATAL("server", "Item slot %u", ((Item*)this)->GetSlot());
        //ASSERT(false);
        RemoveFromWorld();
    }

    if (m_objectUpdated)
    {
        TC_LOG_FATAL("server", "Object::~Object - guid=" UI64FMTD ", typeid=%d, entry=%u deleted but still in update list!!", GetGUID(), GetTypeId(), GetEntry());
        //ASSERT(false);
        sObjectAccessor->RemoveUpdateObject(this);
    }

    delete [] m_uint32Values;
    delete [] _changedFields;

    for(size_t i = 0; i < m_dynamicTab.size(); ++i)
        delete [] m_dynamicTab[i];
}

void Object::_InitValues()
{
    m_uint32Values = new uint32[m_valuesCount];
    memset(m_uint32Values, 0, m_valuesCount*sizeof(uint32));

    _changedFields = new bool[m_valuesCount];
    memset(_changedFields, 0, m_valuesCount*sizeof(bool));

    for(size_t i = 0; i < m_dynamicTab.size(); ++i)
    {
        memset(m_dynamicTab[i], 0, 32*sizeof(uint32));
        m_dynamicChange[i] = false;
    }

    m_objectUpdated = false;
}

void Object::_Create(uint32 guidlow, uint32 entry, HighGuid guidhigh)
{
    if (!m_uint32Values) _InitValues();

    uint64 guid = MAKE_NEW_GUID(guidlow, entry, guidhigh);
    SetUInt64Value(OBJECT_FIELD_GUID, guid);
    SetUInt16Value(OBJECT_FIELD_TYPE, 0, m_objectType);
    m_PackGUID.clear();
    m_PackGUID.appendPackGUID(GetGUID());
    if (GetVignetteId())
        vignetteGuid = MAKE_NEW_GUID(guidlow, 0, HIGHGUID_PLAYER);
}

std::string Object::_ConcatFields(uint16 startIndex, uint16 size) const
{
    std::ostringstream ss;
    for (uint16 index = 0; index < size; ++index)
        ss << GetUInt32Value(index + startIndex) << ' ';
    return ss.str();
}

void Object::AddToWorld()
{
    if (m_inWorld)
        return;

    ASSERT(m_uint32Values);

    m_inWorld = 1;

    // synchronize values mirror with values array (changes will send in updatecreate opcode any way
    ClearUpdateMask(true);
}

void Object::RemoveFromWorld()
{
    if (!m_inWorld)
        return;

    m_inWorld = 0;

    // if we remove from world then sending changes not required
    ClearUpdateMask(true);
}

void Object::BuildCreateUpdateBlockForPlayer(UpdateData* data, Player* target) const
{
    if (!target)
        return;

    uint8  updateType = UPDATETYPE_CREATE_OBJECT;
    uint16 flags      = m_updateFlag;

    uint32 valCount = m_valuesCount;

    /** lower flag1 **/
    if (target == this)                                      // building packet for yourself
        flags |= UPDATEFLAG_SELF;
    else if (GetTypeId() == TYPEID_PLAYER)
        valCount = PLAYER_END_NOT_SELF;

    switch (GetGUIDHigh())
    {
        case HIGHGUID_PLAYER:
        case HIGHGUID_PET:
        case HIGHGUID_CORPSE:
        case HIGHGUID_DYNAMICOBJECT:
        case HIGHGUID_AREATRIGGER:
            updateType = UPDATETYPE_CREATE_OBJECT2;
            break;
        case HIGHGUID_UNIT:
            if (auto unit = ToUnit())
            {
                if (auto summon = unit->ToTempSummon())
                    if (IS_PLAYER_GUID(summon->GetSummonerGUID()))
                        updateType = UPDATETYPE_CREATE_OBJECT2;
            }
            break;
        case HIGHGUID_GAMEOBJECT:
            if (IS_PLAYER_GUID(ToGameObject()->GetOwnerGUID()))
                updateType = UPDATETYPE_CREATE_OBJECT2;
            break;
    }

    if (flags & UPDATEFLAG_STATIONARY_POSITION)
    {
        // UPDATETYPE_CREATE_OBJECT2 for some gameobject types...
        if (isType(TYPEMASK_GAMEOBJECT))
        {
            switch (((GameObject*)this)->GetGoType())
            {
                case GAMEOBJECT_TYPE_TRAP:
                case GAMEOBJECT_TYPE_DUEL_ARBITER:
                case GAMEOBJECT_TYPE_FLAGSTAND:
                case GAMEOBJECT_TYPE_FLAGDROP:
                    updateType = UPDATETYPE_CREATE_OBJECT2;
                    break;
                case GAMEOBJECT_TYPE_TRANSPORT:
                    flags |= UPDATEFLAG_TRANSPORT;
                    break;
                default:
                    break;
            }
        }
    }

    // if (!(flags & UPDATEFLAG_LIVING))
        // if (WorldObject const* worldObject = dynamic_cast<WorldObject const*>(this))
            // if (!worldObject->m_movementInfo.transportGUID.IsEmpty())
                // flags |= UPDATEFLAG_GO_TRANSPORT_POSITION;

    if (ToUnit() && ToUnit()->getVictim())
        flags |= UPDATEFLAG_HAS_TARGET;

    ByteBuffer buf(500);
    buf << uint8(updateType);
    buf.append(GetPackGUID());
    buf << uint8(m_objectTypeId);

    _BuildMovementUpdate(&buf, flags);

    UpdateMask updateMask;
    updateMask.SetCount(valCount);
    _SetCreateBits(&updateMask, target);
    _BuildValuesUpdate(updateType, &buf, &updateMask, target);
    _BuildDynamicValuesUpdate(updateType, &buf, target);

    data->AddUpdateBlock(buf);
}

void Object::SendUpdateToPlayer(Player* player)
{
    // send create update to player
    UpdateData upd(player->GetMapId());
    WorldPacket packet;

    if (player->HaveAtClient((WorldObject*)this))
        BuildValuesUpdateBlockForPlayer(&upd, player);
    else
        BuildCreateUpdateBlockForPlayer(&upd, player);
    if (upd.BuildPacket(&packet))
        player->GetSession()->SendPacket(&packet);
}

void Object::BuildValuesUpdateBlockForPlayer(UpdateData* data, Player* target) const
{
    ByteBuffer buf(500);

    buf << uint8(UPDATETYPE_VALUES);
    buf.append(GetPackGUID());

    UpdateMask updateMask;
    uint32 valCount = m_valuesCount;
    if (GetTypeId() == TYPEID_PLAYER && target != this)
        valCount = PLAYER_END_NOT_SELF;

    updateMask.SetCount(valCount);

    _SetUpdateBits(&updateMask, target);
    _BuildValuesUpdate(UPDATETYPE_VALUES, &buf, &updateMask, target);
    _BuildDynamicValuesUpdate(UPDATETYPE_VALUES, &buf, target);

    data->AddUpdateBlock(buf);
}

void Object::BuildOutOfRangeUpdateBlock(UpdateData* data) const
{
    data->AddOutOfRangeGUID(GetGUID());
}

void Object::DestroyForPlayer(Player* target, bool onDeath) const
{
    ASSERT(target);
    ObjectGuid guid = GetObjectGuid();

    WorldPacket data(SMSG_DESTROY_OBJECT, 8 + 1);
    data.WriteGuidMask<7, 2, 6, 3, 1, 4>(guid);
    //! If the following bool is true, the client will call "void CGUnit_C::OnDeath()" for this object.
    //! OnDeath() does for eg trigger death animation and interrupts certain spells/missiles/auras/sounds...
    data.WriteBit(onDeath);
    data.WriteGuidMask<5, 0>(guid);
    data.WriteGuidBytes<4, 3, 2, 7, 0, 1, 6, 5>(guid);
    target->GetSession()->SendPacket(&data);
}

void Object::_BuildMovementUpdate(ByteBuffer* data, uint16 flags) const
{
    data->WriteBit(0);                              // byte2AC
    data->WriteBit(flags & UPDATEFLAG_AREA_TRIGGER);// byte29C
    data->WriteBit(0);                              // byte1
    data->WriteBit(flags & UPDATEFLAG_TRANSPORT);
    data->WriteBit(flags & UPDATEFLAG_HAS_WORLDEFFECTID); // WorldEffectID
    data->WriteBit(flags & UPDATEFLAG_SELF);
    data->WriteBit(0);                              // byte0
    data->WriteBit(flags & UPDATEFLAG_HAS_TARGET);

    std::vector<uint32> transportFrames;
    if (GameObject const* go = ToGameObject())
    {
        if (go->HasManualAnim())
        {
            GameObjectTemplate const* goInfo = go->GetGOInfo();
            if (goInfo->type == GAMEOBJECT_TYPE_TRANSPORT)
            {
                if (goInfo->transport.Timeto2ndfloor)
                    transportFrames.push_back(goInfo->transport.Timeto2ndfloor);
                if (goInfo->transport.Timeto3rdfloor)
                    transportFrames.push_back(goInfo->transport.Timeto3rdfloor);
                //if (goInfo->transport.Timeto4thfloor)
                //    transportFrames.push_back(goInfo->transport.Timeto4thfloor);
                //if (goInfo->transport.Timeto5thfloor)
                //    transportFrames.push_back(goInfo->transport.Timeto5thfloor);
            }
        }
    }

    data->WriteBits(transportFrames.size(), 22);   // transport animation frames                   

    data->WriteBit(0);                              // byte414
    data->WriteBit(0);                              // byte3
    data->WriteBit(0);                              // byte428
    data->WriteBit(0);                              // byte32A
    data->WriteBit(flags & UPDATEFLAG_STATIONARY_POSITION);
    data->WriteBit(0);                              // PlayHoverAnim?
    data->WriteBit(flags & UPDATEFLAG_LIVING);
    data->WriteBit(flags & UPDATEFLAG_ANIMKITS);
    data->WriteBit(flags & UPDATEFLAG_VEHICLE);
    data->WriteBit(flags & UPDATEFLAG_GO_TRANSPORT_POSITION);
    data->WriteBit(0);                              // is scene object
    data->WriteBit(flags & UPDATEFLAG_ROTATION);    // has gameobject rotation

    if (flags & UPDATEFLAG_AREA_TRIGGER)
    {
        AreaTrigger const* t = ToAreaTrigger();
        ASSERT(t);

        data->WriteBit(t->isPolygon());  // areaTriggerPolygon
        if (t->isPolygon())
        {
            uint32 size = t->GetAreaTriggerInfo().polygonPoints.size();
            data->WriteBits(size, 21); // VerticesCount dword25C
            data->WriteBits(t->GetAreaTriggerInfo().polygon > 1 ? size : 0, 21); // VerticesTargetCount dword26C
        }

        data->WriteBit(t->GetAreaTriggerInfo().HasAbsoluteOrientation);   // HasAbsoluteOrientation?
        data->WriteBit(t->GetAreaTriggerInfo().HasFollowsTerrain);        // HasFollowsTerrain?
        data->WriteBit(t->GetVisualScale());                              // areaTriggerSphere
        data->WriteBit(t->isMoving());                                    // areaTriggerSpline
        data->WriteBit(t->GetAreaTriggerInfo().HasFaceMovementDir);       // HasFaceMovementDir?
        data->WriteBit(t->GetAreaTriggerInfo().HasAttached);              // HasAttached?
        data->WriteBit(t->GetAreaTriggerInfo().ScaleCurveID);             // hasScaleCurveID?
        data->WriteBit(t->GetAreaTriggerInfo().MorphCurveID);             // hasMorphCurveID?
        if (t->isMoving())
            data->WriteBits(t->GetObjectMovementParts(), 20);             // splinePointsCount
        data->WriteBit(t->GetAreaTriggerInfo().FacingCurveID);            // hasFacingCurveID?
        data->WriteBit(t->GetAreaTriggerInfo().HasDynamicShape);          // HasDynamicShape?
        data->WriteBit(t->GetAreaTriggerInfo().MoveCurveID);              // hasMoveCurveID
        data->WriteBit(t->GetAreaTriggerCylinder());                      // areaTriggerCylinder
    }

    if (flags & UPDATEFLAG_LIVING)
    {
        Unit const* self = ToUnit();
        ObjectGuid guid = GetGUID();
        uint32 movementFlags = self->m_movementInfo.GetMovementFlags();
        uint16 movementFlagsExtra = self->m_movementInfo.GetExtraMovementFlags();
        bool hasMoveIndex = self->m_movementInfo.moveIndex != 0;
        // these break update packet
        {
            if (GetTypeId() == TYPEID_UNIT)
                movementFlags &= MOVEMENTFLAG_MASK_CREATURE_ALLOWED;
            else
            {
                if (movementFlags & (MOVEMENTFLAG_FLYING | MOVEMENTFLAG_CAN_FLY))
                    movementFlags &= ~(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR | MOVEMENTFLAG_FALLING_SLOW);
                if ((movementFlagsExtra & MOVEMENTFLAG2_INTERPOLATED_TURNING) == 0)
                    movementFlags &= ~MOVEMENTFLAG_FALLING;
            }
        }

        ObjectGuid transGuid = self->m_movementInfo.transportGUID;
        data->WriteGuidMask<4, 1>(guid);
        data->WriteBits(0, 19);                     // movementForcesCount
        data->WriteGuidMask<5>(guid);
        data->WriteBit(G3D::fuzzyEq(self->GetOrientation(), 0.0f));         // has orientation
        data->WriteGuidMask<7>(guid);
        data->WriteBits(0, 22);                     // removeForcesCount
        data->WriteBit(self->IsSplineEnabled());    // has spline data
        data->WriteBit(!self->m_movementInfo.hasPitch);   // has pitch

        if (self->IsSplineEnabled())
            Movement::PacketBuilder::WriteCreateBits(*self->movespline, *data);

        data->WriteBit(!hasMoveIndex);      // !hasMoveIndex
        data->WriteGuidMask<3>(guid);
        data->WriteBit(self->m_movementInfo.remoteTimeValid); // remoteTimeValid
        data->WriteBit(!movementFlags);
        if (movementFlags)
            data->WriteBits(movementFlags, 30);
        data->WriteBit(self->m_movementInfo.hasSpline);       // hasSpline
        data->WriteBit(self->m_movementInfo.heightChangeFailed);  // heightChangeFailed
        data->WriteGuidMask<2>(guid);
        data->WriteBit(!self->m_movementInfo.hasMoveTime);                      // !hasMoveTime
        data->WriteGuidMask<0>(guid);
        data->WriteBit(transGuid);                  // has transport data

        if (transGuid)
        {
            data->WriteGuidMask<4, 7, 3, 1, 6>(transGuid);
            data->WriteBit(self->m_movementInfo.transportPrevMoveTime);
            data->WriteGuidMask<2, 0, 5>(transGuid);
            data->WriteBit(self->m_movementInfo.transportVehicleRecID);
        }

        data->WriteGuidMask<6>(guid);
        data->WriteBit(self->m_movementInfo.hasFallData); // has fall data
        if (self->m_movementInfo.hasFallData)
            data->WriteBit(self->m_movementInfo.hasFallDirection);           // has fall direction
        data->WriteBit(!movementFlagsExtra);
        data->WriteBit(!self->m_movementInfo.hasStepUpStartElevation);   // has spline elevation
        if (movementFlagsExtra)
            data->WriteBits(movementFlagsExtra, 13);
    }

    if (flags & UPDATEFLAG_GO_TRANSPORT_POSITION)
    {
        WorldObject const* self = static_cast<WorldObject const*>(this);
        ObjectGuid transGuid = self->m_movementInfo.transportGUID;

        data->WriteGuidMask<0, 7>(transGuid);
        data->WriteBit(self->m_movementInfo.transportVehicleRecID);   // has go transport time 3
        data->WriteGuidMask<1>(transGuid);
        data->WriteBit(self->m_movementInfo.transportPrevMoveTime);   // has go transport time 2
        data->WriteGuidMask<6, 5, 4, 3, 2>(transGuid);
    }

    if (flags & UPDATEFLAG_HAS_TARGET)
    {
        ObjectGuid victimGuid = ToUnit()->getVictim()->GetGUID();
        data->WriteGuidMask<5, 4, 6, 0, 1, 7, 2, 3>(victimGuid);
    }

    if (flags & UPDATEFLAG_ANIMKITS)
    {
        data->WriteBit(1);                              // Missing AnimKit3
        data->WriteBit(1);                              // Missing AnimKit1
        data->WriteBit(1);                              // Missing AnimKit2
    }

    data->FlushBits();

    for (uint32 i = 0; i < transportFrames.size(); ++i)
        *data << uint32(transportFrames[i]);

    if (flags & UPDATEFLAG_AREA_TRIGGER)
    {
        AreaTrigger const* t = ToAreaTrigger();
        ASSERT(t);

        if (t->GetAreaTriggerCylinder())                // areaTriggerCylinder
        {
            *data << t->GetAreaTriggerInfo().Height; // Height (float250)
            *data << t->GetAreaTriggerInfo().Float4; // Float4 (float254)
            *data << t->GetAreaTriggerInfo().Float5; // Float5 (float248)
            *data << t->GetAreaTriggerInfo().Radius; // Radius (float24C)
            *data << t->GetAreaTriggerInfo().RadiusTarget; // RadiusTarget (float240)
            *data << t->GetAreaTriggerInfo().HeightTarget; // HeightTarget (float244)
        }

        if (t->isPolygon())
        {
            *data << t->GetAreaTriggerInfo().HeightTarget; // HeightTarget (float280)

            for (uint16 i = 0; i < t->GetAreaTriggerInfo().polygonPoints.size(); ++i)
            {
                *data << t->GetAreaTriggerInfo().polygonPoints[i].y; // Y
                *data << t->GetAreaTriggerInfo().polygonPoints[i].x; // X
            }

            *data << t->GetAreaTriggerInfo().Height; // Height (float27C)

            if(t->GetAreaTriggerInfo().polygon > 1)
            {
                for (uint16 i = 0; i < t->GetAreaTriggerInfo().polygonPoints.size(); ++i)
                {
                    *data << t->GetAreaTriggerInfo().polygonPoints[i].x; // X
                    *data << t->GetAreaTriggerInfo().polygonPoints[i].y; // Y
                }
            }
        }

        if (t->GetAreaTriggerInfo().MoveCurveID)
            *data << uint32(t->GetAreaTriggerInfo().MoveCurveID);

        if (t->GetAreaTriggerInfo().MorphCurveID)
            *data << uint32(t->GetAreaTriggerInfo().MorphCurveID);

        if (t->GetVisualScale())                // areaTriggerSphere
        {
            *data << t->GetVisualScale(true);   // Radius float238
            *data << t->GetVisualScale();       // RadiusTarget float234
        }

        if (t->isMoving())                      // areaTriggerSpline
            t->PutObjectUpdateMovement(data);   // Points

        if(t->GetAreaTriggerInfo().ElapsedTime)
            *data << uint32(t->GetAreaTriggerInfo().ElapsedTime);                     // Elapsed Time Ms
        else
            *data << uint32(1);                     // Elapsed Time Ms

        if (t->GetAreaTriggerInfo().FacingCurveID)
            *data << uint32(t->GetAreaTriggerInfo().FacingCurveID);

        if (t->GetAreaTriggerInfo().ScaleCurveID)
            *data << uint32(t->GetAreaTriggerInfo().ScaleCurveID);
    }

    if (flags & UPDATEFLAG_LIVING)
    {
        Unit const* self = ToUnit();
        ObjectGuid guid = GetGUID();
        uint32 movementFlags = self->m_movementInfo.GetMovementFlags();
        uint16 movementFlagsExtra = self->m_movementInfo.GetExtraMovementFlags();
        bool hasMoveIndex = self->m_movementInfo.moveIndex != 0;
        if (GetTypeId() == TYPEID_UNIT)
            movementFlags &= MOVEMENTFLAG_MASK_CREATURE_ALLOWED;
        ObjectGuid transGuid = self->m_movementInfo.transportGUID;

        *data << float(self->GetPositionY());
        if (self->IsSplineEnabled())
            Movement::PacketBuilder::WriteCreateData(*self->movespline, *data);
        *data << self->GetSpeed(MOVE_FLIGHT);
        *data << self->GetSpeed(MOVE_RUN);
        data->WriteGuidBytes<4>(guid);
        *data << self->GetSpeed(MOVE_WALK);

        if (self->m_movementInfo.hasFallData)
        {
            if (self->m_movementInfo.hasFallDirection)
            {
                *data << float(self->m_movementInfo.fallSpeed);
                *data << float(self->m_movementInfo.fallCosAngle);
                *data << float(self->m_movementInfo.fallSinAngle);
            }

            *data << uint32(self->m_movementInfo.fallTime);
            *data << float(self->m_movementInfo.fallJumpVelocity);
        }

        if (transGuid)
        {
            data->WriteGuidBytes<5>(transGuid);
            *data << int8(self->GetTransSeat());
            data->WriteGuidBytes<2>(transGuid);
            *data << float(Position::NormalizeOrientation(self->GetTransOffsetO()));
            data->WriteGuidBytes<4, 7>(transGuid);
            if (uint32 prevMoveTime = self->m_movementInfo.transportPrevMoveTime)
                *data << uint32(prevMoveTime);
            *data << uint32(self->GetTransTime());
            *data << float(self->GetTransOffsetY());
            data->WriteGuidBytes<3, 6>(transGuid);
            *data << float(self->GetTransOffsetX());
            data->WriteGuidBytes<0>(transGuid);
            if (uint32 vehicleRecID = self->m_movementInfo.transportVehicleRecID)
                *data << uint32(vehicleRecID);
            data->WriteGuidBytes<1>(transGuid);
            *data << float(self->GetTransOffsetZ());
        }

        data->WriteGuidBytes<5>(guid);
        if (self->m_movementInfo.hasMoveTime)
            *data << uint32(getMSTime());
        if (hasMoveIndex)
            *data << uint32(self->m_movementInfo.moveIndex);
        data->WriteGuidBytes<1>(guid);
        *data << self->GetSpeed(MOVE_SWIM_BACK);
        *data << self->GetSpeed(MOVE_FLIGHT_BACK);
        data->WriteGuidBytes<6>(guid);
        *data << self->GetSpeed(MOVE_TURN_RATE);
        *data << float(self->GetPositionX());
        if (!G3D::fuzzyEq(self->GetOrientation(), 0.0f))
            *data << float(Position::NormalizeOrientation(self->GetOrientation()));
        *data << self->GetSpeed(MOVE_PITCH_RATE);
        *data << self->GetSpeed(MOVE_SWIM);
        if (self->m_movementInfo.hasPitch)
            *data << float(Position::NormalizePitch(self->m_movementInfo.pitch));
        data->WriteGuidBytes<3>(guid);
        if (self->m_movementInfo.hasStepUpStartElevation)
            *data << float(self->m_movementInfo.stepUpStartElevation);
        *data << self->GetSpeed(MOVE_RUN_BACK);
        data->WriteGuidBytes<7, 2>(guid);
        *data << float(self->GetPositionZ());
        data->WriteGuidBytes<0>(guid);
    }

    if (flags & UPDATEFLAG_STATIONARY_POSITION)
    {
        WorldObject const* self = static_cast<WorldObject const*>(this);

        *data << float(self->GetPositionX());
        *data << float(self->GetPositionZ());
        *data << float(self->GetPositionY());
        *data << float(Position::NormalizeOrientation(self->GetOrientation()));
    }

    if (flags & UPDATEFLAG_GO_TRANSPORT_POSITION)
    {
        WorldObject const* self = static_cast<WorldObject const*>(this);
        ObjectGuid transGuid = self->m_movementInfo.transportGUID;

        if (uint32 prevMoveTime = self->m_movementInfo.transportPrevMoveTime)
            *data << uint32(prevMoveTime);
        data->WriteGuidBytes<4, 2, 7, 3>(transGuid);
        *data << uint32(self->GetTransTime());
        *data << float(self->GetTransOffsetY());
        data->WriteGuidBytes<1>(transGuid);
        *data << float(self->GetTransOffsetZ());
        *data << int8(self->GetTransSeat());
        if (uint32 vehicleRecID = self->m_movementInfo.transportVehicleRecID)
            *data << uint32(vehicleRecID);
        data->WriteGuidBytes<6>(transGuid);
        *data << float(Position::NormalizeOrientation(self->GetTransOffsetO()));
        data->WriteGuidBytes<5, 0>(transGuid);
        *data << float(self->GetTransOffsetX());
    }

    if (flags & UPDATEFLAG_ROTATION)
        *data << uint64(ToGameObject()->GetRotation());

    if (flags & UPDATEFLAG_HAS_TARGET)
    {
        ObjectGuid victimGuid = ToUnit()->getVictim()->GetGUID();   // checked in BuildCreateUpdateBlockForPlayer
        data->WriteGuidBytes<4, 6, 3, 0, 7, 1, 2, 5>(victimGuid);
    }

    if (flags & UPDATEFLAG_ANIMKITS)
    {
        /*if (hasAnimKit3)
            *data << uint16(0);
        if (hasAnimKit1)
            *data << uint16(0);
        if (hasAnimKit2)
            *data << uint16(0);
        */
    }

    if (flags & UPDATEFLAG_TRANSPORT)
        *data << uint32(getMSTime());

    if (flags & UPDATEFLAG_VEHICLE)
    {
        Unit const* self = ToUnit();

        *data << uint32(self->GetVehicleKit()->GetVehicleInfo()->m_ID);
        *data << float(self->GetOrientation());
    }

    if (flags & UPDATEFLAG_HAS_WORLDEFFECTID)
    {
        if(GameObject const* go = ToGameObject())
            *data << uint32(go->GetGOInfo()->WorldEffectID);
        else
            *data << uint32(0);
    }

    if (flags & UPDATEFLAG_LIVING)
    {
        Unit const* self = ToUnit();

        if (self->IsSplineEnabled())
            Movement::PacketBuilder::WriteFacingData(*self->movespline, *data);
    }
}

void Object::_BuildValuesUpdate(uint8 updatetype, ByteBuffer* data, UpdateMask* updateMask, Player* target) const
{
    if (!target)
        return;

    bool IsActivateToQuest = false;
    if (updatetype == UPDATETYPE_CREATE_OBJECT || updatetype == UPDATETYPE_CREATE_OBJECT2)
    {
        if (isType(TYPEMASK_GAMEOBJECT) && !((GameObject*)this)->IsDynTransport())
        {
            if (((GameObject*)this)->ActivateToQuest(target))
                IsActivateToQuest = true;

            if (((GameObject*)this)->GetGoArtKit())
                updateMask->SetBit(GAMEOBJECT_BYTES_1);
        }
        else if (isType(TYPEMASK_UNIT))
        {
            if (((Unit*)this)->HasFlag(UNIT_FIELD_AURASTATE, PER_CASTER_AURA_STATE_MASK))
                updateMask->SetBit(UNIT_FIELD_AURASTATE);
        }
    }
    else                                                    // case UPDATETYPE_VALUES
    {
        if (isType(TYPEMASK_GAMEOBJECT) && !((GameObject*)this)->IsTransport())
        {
            if (((GameObject*)this)->ActivateToQuest(target))
                IsActivateToQuest = true;

            updateMask->SetBit(GAMEOBJECT_BYTES_1);

            if (ToGameObject()->GetGoType() == GAMEOBJECT_TYPE_CHEST && ToGameObject()->GetGOInfo()->chest.usegrouplootrules &&
                ToGameObject()->HasLootRecipient())
                updateMask->SetBit(GAMEOBJECT_FLAGS);
        }
        else if (isType(TYPEMASK_UNIT))
        {
            if (((Unit*)this)->HasFlag(UNIT_FIELD_AURASTATE, PER_CASTER_AURA_STATE_MASK))
                updateMask->SetBit(UNIT_FIELD_AURASTATE);
        }
    }

    uint32 valCount = m_valuesCount;
    if (GetTypeId() == TYPEID_PLAYER && target != this)
        valCount = PLAYER_END_NOT_SELF;

    WPAssert(updateMask && updateMask->GetCount() == valCount);

    *data << (uint8)updateMask->GetBlockCount();
    data->append(updateMask->GetMask(), updateMask->GetLength());

    // 2 specialized loops for speed optimization in non-unit case
    if (isType(TYPEMASK_UNIT))                               // unit (creature/player) case
    {
        for (uint16 index = 0; index < valCount; ++index)
        {
            if (updateMask->GetBit(index))
            {
                if (index == UNIT_NPC_FLAGS)
                {
                    // remove custom flag before sending
                    uint32 appendValue = m_uint32Values[index];

                    if (GetTypeId() == TYPEID_UNIT)
                    {
                        if (!target->canSeeSpellClickOn(this->ToCreature()))
                            appendValue &= ~UNIT_NPC_FLAG_SPELLCLICK;

                        if (appendValue & UNIT_NPC_FLAG_TRAINER)
                        {
                            if (!this->ToCreature()->isCanTrainingOf(target, false))
                                appendValue &= ~(UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS | UNIT_NPC_FLAG_TRAINER_PROFESSION);
                        }
                    }

                    *data << uint32(appendValue);
                }
                else if (index == UNIT_FIELD_AURASTATE)
                {
                    // Check per caster aura states to not enable using a pell in client if specified aura is not by target
                    *data << ((Unit*)this)->BuildAuraStateUpdateForTarget(target);
                }
                else if (index == UNIT_FIELD_MAXDAMAGE || index == UNIT_FIELD_MINDAMAGE || index == UNIT_FIELD_MINOFFHANDDAMAGE || index == UNIT_FIELD_MAXOFFHANDDAMAGE)
                {
                    *data << (m_floatValues[index] + CalculatePct(m_floatValues[index], ((Unit*)this)->GetTotalAuraModifier(SPELL_AURA_MOD_AUTOATTACK_DAMAGE)));
                }
                // FIXME: Some values at server stored in float format but must be sent to client in uint32 format
                else if (index >= UNIT_FIELD_BASEATTACKTIME && index <= UNIT_FIELD_RANGEDATTACKTIME)
                {
                    // convert from float to uint32 and send
                    *data << uint32(m_floatValues[index] < 0 ? 0 : (RoundingFloatValue(m_floatValues[index] / 10) * 10));
                }
                // there are some float values which may be negative or can't get negative due to other checks
                else if ((index >= UNIT_FIELD_NEGSTAT0 && index <= UNIT_FIELD_NEGSTAT0+4) ||
                    (index >= UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE && index <= (UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE + 6)) ||
                    (index >= UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE && index <= (UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE + 6)) ||
                    (index >= UNIT_FIELD_POSSTAT0 && index <= UNIT_FIELD_POSSTAT0 + 4))
                {
                    *data << uint32(m_floatValues[index]);
                }
                // Gamemasters should be always able to select units - remove not selectable flag
                else if (index == UNIT_FIELD_FLAGS)
                {
                    if (target->isGameMaster())
                        *data << (m_uint32Values[index] & ~UNIT_FLAG_NOT_SELECTABLE);
                    else
                        *data << m_uint32Values[index];
                }
                // use modelid_a if not gm, _h if gm for CREATURE_FLAG_EXTRA_TRIGGER creatures
                else if (index == UNIT_FIELD_DISPLAYID)
                {
                    if (GetTypeId() == TYPEID_UNIT)
                    {
                        CreatureModelInfo const* modelInfo = sObjectMgr->GetCreatureModelInfo(m_uint32Values[index]);
                        CreatureTemplate const* cinfo = ToCreature()->GetCreatureTemplate();

                        // this also applies for transform auras
                        if (SpellInfo const* transform = sSpellMgr->GetSpellInfo(ToUnit()->getTransForm()))
                            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                                if (transform->Effects[i]->IsAura(SPELL_AURA_TRANSFORM))
                                    if (CreatureTemplate const* transformInfo = sObjectMgr->GetCreatureTemplate(transform->Effects[i]->MiscValue))
                                    {
                                        cinfo = transformInfo;
                                        break;
                                    }

                        if(modelInfo && modelInfo->hostileId && ToUnit()->IsHostileTo(target))
                            *data << modelInfo->hostileId;
                        else if (cinfo->flags_extra & CREATURE_FLAG_EXTRA_TRIGGER)
                        {
                            if (target->isGameMaster())
                            {
                                if (cinfo->Modelid1)
                                    *data << cinfo->Modelid1;//Modelid1 is a visible model for gms
                                else
                                    *data << 17519; // world invisible trigger's model
                            }
                            else
                            {
                                if (cinfo->Modelid2)
                                    *data << cinfo->Modelid2;//Modelid2 is an invisible model for players
                                else
                                    *data << 11686; // world invisible trigger's model
                            }
                        }
                        else
                            *data << m_uint32Values[index];
                    }
                    else
                        *data << m_uint32Values[index];
                }
                // hide lootable animation for unallowed players
                else if (index == OBJECT_FIELD_DYNAMIC_FLAGS)
                {
                    uint32 dynamicFlags = m_uint32Values[index];

                    if (Creature const* creature = ToCreature())
                    {
                        if (creature->hasLootRecipient())
                        {
                            if(creature->IsPersonalLoot())
                            {
                                dynamicFlags |= (UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_PLAYER | UNIT_DYNFLAG_TAPPED_BY_ALL_THREAT_LIST);
                            }
                            else if (creature->isTappedBy(target))
                            {
                                dynamicFlags |= (UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_PLAYER);
                            }
                            else
                            {
                                dynamicFlags |= UNIT_DYNFLAG_TAPPED;
                                dynamicFlags &= ~UNIT_DYNFLAG_TAPPED_BY_PLAYER;
                            }
                        }
                        else
                        {
                            dynamicFlags &= ~UNIT_DYNFLAG_TAPPED;
                            dynamicFlags &= ~UNIT_DYNFLAG_TAPPED_BY_PLAYER;
                        }

                        if (!target->isAllowedToLoot(const_cast<Creature*>(creature)))
                            dynamicFlags &= ~UNIT_DYNFLAG_LOOTABLE;
                    }

                    // unit UNIT_DYNFLAG_TRACK_UNIT should only be sent to caster of SPELL_AURA_MOD_STALKED auras
                    if (Unit const* unit = ToUnit())
                        if (dynamicFlags & UNIT_DYNFLAG_TRACK_UNIT)
                            if (!unit->HasAuraTypeWithCaster(SPELL_AURA_MOD_STALKED, target->GetGUID()))
                                dynamicFlags &= ~UNIT_DYNFLAG_TRACK_UNIT;
                    *data << dynamicFlags;
                }
                // FG: pretend that OTHER players in own group are friendly ("blue")
                else if (index == UNIT_FIELD_BYTES_2 || index == UNIT_FIELD_FACTIONTEMPLATE)
                {
                    Unit const* unit = ToUnit();
                    if (!unit->HasAuraType(SPELL_AURA_MOD_FACTION) && !unit->HasAura(119626) && unit->IsControlledByPlayer() && target != this && sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP) && unit->IsInRaidWith(target))
                    {
                        FactionTemplateEntry const* ft1 = unit->getFactionTemplateEntry();
                        FactionTemplateEntry const* ft2 = target->getFactionTemplateEntry();
                        if (ft1 && ft2 && !ft1->IsFriendlyTo(*ft2))
                        {
                            if (index == UNIT_FIELD_BYTES_2)
                            {
                                // Allow targetting opposite faction in party when enabled in config
                                *data << (m_uint32Values[index] & ((UNIT_BYTE2_FLAG_SANCTUARY /*| UNIT_BYTE2_FLAG_AURAS | UNIT_BYTE2_FLAG_UNK5*/) << 8)); // this flag is at uint8 offset 1 !!
                            }
                            else
                            {
                                // pretend that all other HOSTILE players have own faction, to allow follow, heal, rezz (trade wont work)
                                uint32 faction = target->getFaction();
                                *data << uint32(faction);
                            }
                        }
                        else
                            *data << m_uint32Values[index];
                    }
                    else
                        *data << m_uint32Values[index];
                }
                else
                {
                    // send in current format (float as float, uint32 as uint32)
                    *data << m_uint32Values[index];
                }
            }
        }
    }
    else if (isType(TYPEMASK_GAMEOBJECT))                    // gameobject case
    {
        for (uint16 index = 0; index < valCount; ++index)
        {
            if (updateMask->GetBit(index))
            {
                // send in current format (float as float, uint32 as uint32)
                if (index == OBJECT_FIELD_DYNAMIC_FLAGS)
                {
                    if (IsActivateToQuest || target->isGameMaster())
                    {
                        switch (ToGameObject()->GetGoType())
                        {
                            case GAMEOBJECT_TYPE_CHEST:
                            case GAMEOBJECT_TYPE_GOOBER:
                                if (!IsActivateToQuest)
                                    *data << uint16(GO_DYNFLAG_LO_ACTIVATE);
                                else
                                    *data << uint16(GO_DYNFLAG_LO_ACTIVATE | GO_DYNFLAG_LO_SPARKLE);
                                break;
                            case GAMEOBJECT_TYPE_GENERIC:
                            case GAMEOBJECT_TYPE_SPELL_FOCUS:
                                if (!IsActivateToQuest)
                                    *data << uint16(0);
                                else
                                    *data << uint16(GO_DYNFLAG_LO_SPARKLE);
                                break;
                            default:
                                *data << uint16(0); // unknown, not happen.
                                break;
                        }
                    }
                    else
                        *data << uint16(0);         // disable quest object

                    *data << uint16(-1);
                }
                else if (index == GAMEOBJECT_FLAGS)
                {
                    uint32 flags = m_uint32Values[index];
                    if (ToGameObject()->GetGoType() == GAMEOBJECT_TYPE_CHEST)
                        if (ToGameObject()->GetGOInfo()->chest.usegrouplootrules && !ToGameObject()->IsLootAllowedFor(target))
                            flags |= GO_FLAG_LOCKED | GO_FLAG_NOT_SELECTABLE;

                    *data << flags;
                }
                else if (index == GAMEOBJECT_BYTES_1)
                {
                    if (((GameObject*)this)->GetGOInfo()->type == GAMEOBJECT_TYPE_TRANSPORT)
                        *data << uint32(m_uint32Values[index] | GO_STATE_TRANSPORT_SPEC);
                    else
                        *data << uint32(m_uint32Values[index]);
                }
                else
                    *data << m_uint32Values[index];                // other cases
            }
        }
    }
    else if (isType(TYPEMASK_DYNAMICOBJECT))                    // dynamiobject case
    {
        for (uint16 index = 0; index < valCount; ++index)
        {
            if (updateMask->GetBit(index))
            {
                if (index == DYNAMICOBJECT_BYTES)
                {
                    uint32 visualId = ((DynamicObject*)this)->GetVisualId();
                    DynamicObjectType dynType = ((DynamicObject*)this)->GetType();
                    Unit* caster = ((DynamicObject*)this)->GetCaster();
                    SpellVisualEntry const* visualEntry = sSpellVisualStore.LookupEntry(visualId);
                    if(caster && visualEntry && visualEntry->hostileId && caster->IsHostileTo(target))
                        *data << ((dynType << 28) | visualEntry->hostileId);
                    else
                        *data << m_uint32Values[index];
                }
                else
                    *data << m_uint32Values[index];
            }
        }
    }
    else if (isType(TYPEMASK_AREATRIGGER))                    // AreaTrigger case
    {
        for (uint16 index = 0; index < valCount; ++index)
        {
            if (updateMask->GetBit(index))
            {
                if (index == AREATRIGGER_SPELLVISUALID)
                {
                    uint32 visualId = m_uint32Values[index];
                    Unit* caster = ((AreaTrigger*)this)->GetCaster();
                    SpellVisualEntry const* visualEntry = sSpellVisualStore.LookupEntry(visualId);
                    if(caster && visualEntry && visualEntry->hostileId && caster->IsHostileTo(target))
                        *data << (visualEntry->hostileId);
                    else
                        *data << m_uint32Values[index];
                }
                else
                    *data << m_uint32Values[index];
            }
        }
    }
    else                                                    // other objects case (no special index checks)
    {
        for (uint16 index = 0; index < valCount; ++index)
        {
            if (updateMask->GetBit(index))
            {
                // send in current format (float as float, uint32 as uint32)
                *data << m_uint32Values[index];
            }
        }
    }
}

void Object::_BuildDynamicValuesUpdate(uint8 updatetype, ByteBuffer *data, Player* target) const
{
    // Crashfix, prevent use of bag with dynamic field
    if (isType(TYPEMAST_BAG) || 
        (updatetype == UPDATETYPE_VALUES && GetTypeId() == TYPEID_PLAYER && this != target))
    {
        *data << uint8(0);
        return;
    }

    uint32 dynamicTabMask = 0;
    std::vector<uint32> dynamicFieldsMask;
    dynamicFieldsMask.resize(m_dynamicTab.size());

    for (size_t i = 0; i < m_dynamicTab.size(); ++i)
    {
        dynamicFieldsMask[i] = 0;
        for (int index = 0; index < 32; ++index)
        {
            if ((updatetype == UPDATETYPE_CREATE_OBJECT || updatetype == UPDATETYPE_CREATE_OBJECT2) ||
                updatetype == UPDATETYPE_VALUES && m_dynamicChange[i])
            {
                dynamicTabMask |= 1 << i;
                if (m_dynamicTab[i][index] != 0)
                    dynamicFieldsMask[i] |= 1 << index;
            }
        }
    }

    *data << uint8(dynamicTabMask ? 1 : 0); // count of dynamic tab masks
    if (dynamicTabMask)
    {
        *data << uint32(dynamicTabMask);

        for (size_t i = 0; i < m_dynamicTab.size(); ++i)
        {
            if (dynamicTabMask & (1 << i))
            {
                *data << uint8(bool(dynamicFieldsMask[i]));      // count of dynamic field masks
                if (dynamicFieldsMask[i])
                {
                    *data << uint32(dynamicFieldsMask[i]);

                    for (int index = 0; index < 32; ++index)
                    {
                        if (dynamicFieldsMask[i] & (1 << index))
                            *data << uint32(m_dynamicTab[i][index]);
                    }
                }
            }
        }
    }
}

void Object::ClearUpdateMask(bool remove)
{
    memset(_changedFields, 0, m_valuesCount*sizeof(bool));

    if (m_objectUpdated)
    {
        for(size_t i = 0; i < m_dynamicTab.size(); i++)
            m_dynamicChange[i] = false;

        if (remove)
            sObjectAccessor->RemoveUpdateObject(this);
        m_objectUpdated = false;
    }
}

void Object::BuildFieldsUpdate(Player* player, UpdateDataMapType& data_map) const
{
    UpdateDataMapType::iterator iter = data_map.find(player);

    if (iter == data_map.end())
    {
        std::pair<UpdateDataMapType::iterator, bool> p = data_map.insert(UpdateDataMapType::value_type(player, UpdateData(player->GetMapId())));
        ASSERT(p.second);
        iter = p.first;
    }

    BuildValuesUpdateBlockForPlayer(&iter->second, iter->first);
}

void Object::_LoadIntoDataField(char const* data, uint32 startOffset, uint32 count)
{
    if (!data)
        return;

    Tokenizer tokens(data, ' ', count);

    if (tokens.size() != count)
        return;

    for (uint32 index = 0; index < count; ++index)
    {
        m_uint32Values[startOffset + index] = atol(tokens[index]);
        _changedFields[startOffset + index] = true;
    }
}

void Object::GetUpdateFieldData(Player const* target, uint32*& flags, bool& isOwner, bool& isItemOwner, bool& hasSpecialInfo, bool& isPartyMember) const
{
    // This function assumes updatefield index is always valid
    switch (GetTypeId())
    {
        case TYPEID_ITEM:
        case TYPEID_CONTAINER:
            flags = ItemUpdateFieldFlags;
            isOwner = isItemOwner = ((Item*)this)->GetOwnerGUID() == target->GetGUID();
            break;
        case TYPEID_UNIT:
        case TYPEID_PLAYER:
        {
            Player* plr = ToUnit()->GetCharmerOrOwnerPlayerOrPlayerItself();
            flags = UnitUpdateFieldFlags;
            isOwner = ToUnit()->GetOwnerGUID() == target->GetGUID();
            hasSpecialInfo = ToUnit()->HasAuraTypeWithCaster(SPELL_AURA_EMPATHY, target->GetGUID());
            isPartyMember = plr && plr->IsInSameGroupWith(target);
            break;
        }
        case TYPEID_GAMEOBJECT:
            flags = GameObjectUpdateFieldFlags;
            isOwner = ToGameObject()->GetOwnerGUID() == target->GetGUID();
            break;
        case TYPEID_DYNAMICOBJECT:
            flags = DynamicObjectUpdateFieldFlags;
            isOwner = ((DynamicObject*)this)->GetCasterGUID() == target->GetGUID();
            break;
        case TYPEID_CORPSE:
            flags = CorpseUpdateFieldFlags;
            isOwner = ToCorpse()->GetOwnerGUID() == target->GetGUID();
            break;
        case TYPEID_AREATRIGGER:
            flags = AreaTriggerUpdateFieldFlags;
            isOwner = ToAreaTrigger()->GetUInt64Value(AREATRIGGER_CASTER) == target->GetGUID();
            break;
        case TYPEID_OBJECT:
            break;
    }
}

bool Object::IsUpdateFieldVisible(uint32 flags, bool isSelf, bool isOwner, bool isItemOwner, bool isPartyMember) const
{
    if (flags == UF_FLAG_NONE)
        return false;

    if (flags & (UF_FLAG_PUBLIC | UF_FLAG_DYNAMIC))
        return true;

    if (flags & UF_FLAG_PRIVATE && isSelf)
        return true;

    if (flags & UF_FLAG_OWNER && (isOwner || isItemOwner))
        return true;

    if (flags & UF_FLAG_PARTY_MEMBER && isPartyMember)
        return true;

    if (flags & UF_FLAG_UNIT_ALL)
        return true;

    return false;
}

void Object::_SetUpdateBits(UpdateMask* updateMask, Player* target) const
{
    bool* indexes = _changedFields;
    uint32* flags = NULL;
    bool isSelf = target == this;
    bool isOwner = false;
    bool isItemOwner = false;
    bool hasSpecialInfo = false;
    bool isPartyMember = false;

    GetUpdateFieldData(target, flags, isOwner, isItemOwner, hasSpecialInfo, isPartyMember);

    uint32 valCount = m_valuesCount;
    if (GetTypeId() == TYPEID_PLAYER && target != this)
        valCount = PLAYER_END_NOT_SELF;

    for (uint16 index = 0; index < valCount; ++index, ++indexes)
        if (_fieldNotifyFlags & flags[index] || (flags[index] & UF_FLAG_SPECIAL_INFO && hasSpecialInfo) || (*indexes && IsUpdateFieldVisible(flags[index], isSelf, isOwner, isItemOwner, isPartyMember)))
            updateMask->SetBit(index);
}

void Object::_SetCreateBits(UpdateMask* updateMask, Player* target) const
{
    uint32* value = m_uint32Values;
    uint32* flags = NULL;
    bool isSelf = target == this;
    bool isOwner = false;
    bool isItemOwner = false;
    bool hasSpecialInfo = false;
    bool isPartyMember = false;

    GetUpdateFieldData(target, flags, isOwner, isItemOwner, hasSpecialInfo, isPartyMember);

    uint32 valCount = m_valuesCount;
    if (GetTypeId() == TYPEID_PLAYER && target != this)
        valCount = PLAYER_END_NOT_SELF;

    for (uint16 index = 0; index < valCount; ++index, ++value)
        if (_fieldNotifyFlags & flags[index] || (flags[index] & UF_FLAG_DYNAMIC) ||(flags[index] & UF_FLAG_SPECIAL_INFO && hasSpecialInfo) || (*value && IsUpdateFieldVisible(flags[index], isSelf, isOwner, isItemOwner, isPartyMember)))
            updateMask->SetBit(index);
}

void Object::SetInt32Value(uint16 index, int32 value)
{
    ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    if (m_int32Values[index] != value)
    {
        m_int32Values[index] = value;
        _changedFields[index] = true;

        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }
    }
}

void Object::UpdateInt32Value(uint16 index, int32 value)
{
    ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    m_int32Values[index] = value;
}

void Object::SetUInt32Value(uint16 index, uint32 value)
{
    ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    if (m_uint32Values[index] != value)
    {
        m_uint32Values[index] = value;
        _changedFields[index] = true;

        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }
    }
}

void Object::UpdateUInt32Value(uint16 index, uint32 value)
{
    ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    m_uint32Values[index] = value;
}

void Object::SetUInt64Value(uint16 index, uint64 value)
{
    ASSERT(index + 1 < m_valuesCount || PrintIndexError(index, true));
    if (*((uint64*)&(m_uint32Values[index])) != value)
    {
        m_uint32Values[index] = PAIR64_LOPART(value);
        m_uint32Values[index + 1] = PAIR64_HIPART(value);
        _changedFields[index] = true;
        _changedFields[index + 1] = true;

        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }
    }
}

bool Object::AddUInt64Value(uint16 index, uint64 value)
{
    ASSERT(index + 1 < m_valuesCount || PrintIndexError(index, true));
    if (value && !*((uint64*)&(m_uint32Values[index])))
    {
        m_uint32Values[index] = PAIR64_LOPART(value);
        m_uint32Values[index + 1] = PAIR64_HIPART(value);
        _changedFields[index] = true;
        _changedFields[index + 1] = true;

        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }

        return true;
    }

    return false;
}

bool Object::RemoveUInt64Value(uint16 index, uint64 value)
{
    ASSERT(index + 1 < m_valuesCount || PrintIndexError(index, true));
    if (value && *((uint64*)&(m_uint32Values[index])) == value)
    {
        m_uint32Values[index] = 0;
        m_uint32Values[index + 1] = 0;
        _changedFields[index] = true;
        _changedFields[index + 1] = true;

        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }

        return true;
    }

    return false;
}

void Object::SetFloatValue(uint16 index, float value)
{
    ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    if (m_floatValues[index] != value)
    {
        m_floatValues[index] = value;
        _changedFields[index] = true;

        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }
    }
}

void Object::SetByteValue(uint16 index, uint8 offset, uint8 value)
{
    ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    if (offset > 4)
    {
        TC_LOG_ERROR("server", "Object::SetByteValue: wrong offset %u", offset);
        return;
    }

    if (uint8(m_uint32Values[index] >> (offset * 8)) != value)
    {
        m_uint32Values[index] &= ~uint32(uint32(0xFF) << (offset * 8));
        m_uint32Values[index] |= uint32(uint32(value) << (offset * 8));
        _changedFields[index] = true;

        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }
    }
}

void Object::SetUInt16Value(uint16 index, uint8 offset, uint16 value)
{
    ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    if (offset > 2)
    {
        TC_LOG_ERROR("server", "Object::SetUInt16Value: wrong offset %u", offset);
        return;
    }

    if (uint16(m_uint32Values[index] >> (offset * 16)) != value)
    {
        m_uint32Values[index] &= ~uint32(uint32(0xFFFF) << (offset * 16));
        m_uint32Values[index] |= uint32(uint32(value) << (offset * 16));
        _changedFields[index] = true;

        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }
    }
}

void Object::SetStatFloatValue(uint16 index, float value)
{
    if (value < 0)
        value = 0.0f;

    SetFloatValue(index, value);
}

void Object::SetStatInt32Value(uint16 index, int32 value)
{
    if (value < 0)
        value = 0;

    SetUInt32Value(index, uint32(value));
}

void Object::ApplyModUInt32Value(uint16 index, int32 val, bool apply)
{
    int32 cur = GetUInt32Value(index);
    cur += (apply ? val : -val);
    if (cur < 0)
        cur = 0;
    SetUInt32Value(index, cur);
}

void Object::ApplyModInt32Value(uint16 index, int32 val, bool apply)
{
    int32 cur = GetInt32Value(index);
    cur += (apply ? val : -val);
    SetInt32Value(index, cur);
}

void Object::ApplyModSignedFloatValue(uint16 index, float  val, bool apply)
{
    float cur = GetFloatValue(index);
    cur += (apply ? val : -val);
    SetFloatValue(index, cur);
}

void Object::ApplyModPositiveFloatValue(uint16 index, float  val, bool apply)
{
    float cur = GetFloatValue(index);
    cur += (apply ? val : -val);
    if (cur < 0)
        cur = 0;
    SetFloatValue(index, cur);
}

void Object::SetFlag(uint16 index, uint32 newFlag)
{
    // ASSERT(index < m_valuesCount || PrintIndexError(index, true));
    if(!(index < m_valuesCount || PrintIndexError(index , true)))
        return;

    uint32 oldval = m_uint32Values[index];
    uint32 newval = oldval | newFlag;

    if (oldval != newval)
    {
        m_uint32Values[index] = newval;
        _changedFields[index] = true;

        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }
    }
}

void Object::RemoveFlag(uint16 index, uint32 oldFlag)
{
    ASSERT(index < m_valuesCount || PrintIndexError(index, true));
    ASSERT(m_uint32Values);

    uint32 oldval = m_uint32Values[index];
    uint32 newval = oldval & ~oldFlag;

    if (oldval != newval)
    {
        m_uint32Values[index] = newval;
        _changedFields[index] = true;

        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }
    }
}

void Object::SetByteFlag(uint16 index, uint8 offset, uint8 newFlag)
{
    ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    if (offset > 4)
    {
        TC_LOG_ERROR("server", "Object::SetByteFlag: wrong offset %u", offset);
        return;
    }

    if (!(uint8(m_uint32Values[index] >> (offset * 8)) & newFlag))
    {
        m_uint32Values[index] |= uint32(uint32(newFlag) << (offset * 8));
        _changedFields[index] = true;

        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }
    }
}

void Object::RemoveByteFlag(uint16 index, uint8 offset, uint8 oldFlag)
{
    ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    if (offset > 4)
    {
        TC_LOG_ERROR("server", "Object::RemoveByteFlag: wrong offset %u", offset);
        return;
    }

    if (uint8(m_uint32Values[index] >> (offset * 8)) & oldFlag)
    {
        m_uint32Values[index] &= ~uint32(uint32(oldFlag) << (offset * 8));
        _changedFields[index] = true;

        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }
    }
}

void Object::SetDynamicUInt32Value(uint32 tab, uint16 index, uint32 value)
{
    //ASSERT(tab < m_dynamicTab.size() && index < 32);
    if(!(tab < m_dynamicTab.size() && index < 32))
    {
        TC_LOG_ERROR("server", "Object::SetDynamicUInt32Value: ASSERT FAILED index %u, tab %u, m_dynamicTab.size() %u", index, tab, m_dynamicTab.size());
        return;
    }

    if (m_dynamicTab[tab][index] != value)
    {
        m_dynamicTab[tab][index] = value;
        m_dynamicChange[tab] = true;
        if (m_inWorld == 1 && !m_objectUpdated)
        {
            sObjectAccessor->AddUpdateObject(this);
            m_objectUpdated = true;
        }
    }
}

bool Object::PrintIndexError(uint32 index, bool set) const
{
    TC_LOG_ERROR("server", "Attempt %s non-existed value field: %u (count: %u) for object typeid: %u type mask: %u", (set ? "set value to" : "get value from"), index, m_valuesCount, GetTypeId(), m_objectType);

    // ASSERT must fail after function call
    return false;
}

bool Position::HasInLine(WorldObject const* target, float width) const
{
    if (!HasInArc(M_PI, target))
        return false;
    width += target->GetObjectSize();
    float angle = GetRelativeAngle(target);
    return fabs(sin(angle)) * GetExactDist2d(target->GetPositionX(), target->GetPositionY()) < width;
}

bool Position::IsInDegreesRange(float x, float y, float degresA, float degresB, bool relative/* = false*/) const
{
    float angel = GetDegreesAngel(x, y, relative);
    return angel >= degresA && angel <= degresB;
}

float Position::GetDegreesAngel(float x, float y, bool relative) const
{
    float angel = relative ? GetRelativeAngle(x, y) : GetAngle(x, y);
    return NormalizeOrientation(angel) * M_RAD;
}

Position Position::GetRandPointBetween(const Position &B) const
{
    float Lambda = urand(0.0f, 100.0f) / 100.0f;
    float X = (B.GetPositionX() + Lambda * GetPositionX()) / (1 + Lambda);
    float Y = (B.GetPositionY() + Lambda * GetPositionY()) / (1 + Lambda);
    //Z should be updated by Vmap
    float Z = (B.GetPositionZ() + Lambda * GetPositionZ()) / (1 + Lambda);

    Position result;
    result.Relocate(X, Y, Z);
    return result;
}

void Position::SimplePosXYRelocationByAngle(Position &pos, float dist, float angle, bool relative) const
{
    if(!relative)
        angle += GetOrientation();

    pos.m_positionX = m_positionX + dist * std::cos(angle);
    pos.m_positionY = m_positionY + dist * std::sin(angle);
    pos.m_positionZ = m_positionZ;

    // Prevent invalid coordinates here, position is unchanged
    if (!Trinity::IsValidMapCoord(pos.m_positionX, pos.m_positionY))
    {
        pos.Relocate(this);
        TC_LOG_FATAL("server", "Position::SimplePosXYRelocationByAngle invalid coordinates X: %f and Y: %f were passed!", pos.m_positionX, pos.m_positionY);
        return;
    }

    Trinity::NormalizeMapCoord(pos.m_positionX);
    Trinity::NormalizeMapCoord(pos.m_positionY);
    pos.SetOrientation(GetOrientation());
}

void Position::SimplePosXYRelocationByAngle(float &x, float &y, float &z, float dist, float angle, bool relative) const
{
    if(!relative)
        angle += GetOrientation();

    x = m_positionX + dist * std::cos(angle);
    y = m_positionY + dist * std::sin(angle);
    z = m_positionZ;

    // Prevent invalid coordinates here, position is unchanged
    if (!Trinity::IsValidMapCoord(x, y))
    {
        x = m_positionX;
        y = m_positionY;
        z = m_positionZ;
        TC_LOG_FATAL("server", "Position::SimplePosXYRelocationByAngle invalid coordinates X: %f and Y: %f were passed!", x, y);
        return;
    }

    Trinity::NormalizeMapCoord(x);
    Trinity::NormalizeMapCoord(y);
}

std::string Position::ToString() const
{
    std::stringstream sstr;
    sstr << "X: " << m_positionX << " Y: " << m_positionY << " Z: " << m_positionZ << " O: " << m_orientation;
    return sstr.str();
}

ByteBuffer& operator>>(ByteBuffer& buf, Position::PositionXYZOStreamer const& streamer)
{
    float x, y, z, o;
    buf >> x >> y >> z >> o;
    streamer.m_pos->Relocate(x, y, z, o);
    return buf;
}
ByteBuffer& operator<<(ByteBuffer& buf, Position::PositionXYZStreamer const& streamer)
{
    float x, y, z;
    streamer.m_pos->GetPosition(x, y, z);
    buf << x << y << z;
    return buf;
}

ByteBuffer& operator>>(ByteBuffer& buf, Position::PositionXYZStreamer const& streamer)
{
    float x, y, z;
    buf >> x >> y >> z;
    streamer.m_pos->Relocate(x, y, z);
    return buf;
}

ByteBuffer& operator<<(ByteBuffer& buf, Position::PositionXYZOStreamer const& streamer)
{
    float x, y, z, o;
    streamer.m_pos->GetPosition(x, y, z, o);
    buf << x << y << z << o;
    return buf;
}

void MovementInfo::OutDebug()
{
    TC_LOG_INFO("network", "MOVEMENT INFO");
    //TC_LOG_INFO("network", "moverGUID " UI64FMTD, moverGUID);
    TC_LOG_INFO("network", "flags %u", flags);
    TC_LOG_INFO("network", "flags2 %u", flags2);
    TC_LOG_INFO("network", "time %u current time " UI64FMTD "", flags2, uint64(::time(NULL)));
    TC_LOG_INFO("network", "position: `%s`", position.ToString().c_str());

    if (transportGUID)
    {
        TC_LOG_INFO("network", "TRANSPORT:");
        //TC_LOG_INFO("network", "guid: " UI64FMTD, transportGUID);
        TC_LOG_INFO("network", "position: `%s`", transportPosition.ToString().c_str());
        TC_LOG_INFO("network", "seatIndex: %i", transportVehicleSeatIndex);
        TC_LOG_INFO("network", "moveTime: %u", transportMoveTime);
        if (hasTransportPrevMoveTime)
            TC_LOG_INFO("network", "prevMoveTime: %u", transportPrevMoveTime);
        if (hasTransportVehicleRecID)
            TC_LOG_INFO("network", "vehicleRecID: %u", transportVehicleRecID);
    }

    TC_LOG_INFO("network", "SWIMMING/FLYING:");
    if (hasPitch)
        TC_LOG_INFO("network", "pitch: %f", pitch);

    if (hasFallData)
    {
        TC_LOG_INFO("network", "FALLING/JUMPING:");
        TC_LOG_INFO("network", "fallTime: %u", fallTime);
        TC_LOG_INFO("network", "jumpVelocity: %f", fallJumpVelocity);
        if (hasFallDirection)
            TC_LOG_INFO("network", "sinAngle: %f, cosAngle: %f, speed: %f", fallSinAngle, fallCosAngle, fallSpeed);
    }

    TC_LOG_INFO("network", "ELEVATION:");
    if (hasStepUpStartElevation)
        TC_LOG_INFO("network", "stepUpStartElevation: %f", stepUpStartElevation);
}

WorldObject::WorldObject(bool isWorldObject): WorldLocation(),
m_name(""), m_isActive(false), m_isWorldObject(isWorldObject), m_zoneScript(NULL),
m_transport(NULL), m_currMap(NULL), m_InstanceId(0),
m_phaseMask(PHASEMASK_NORMAL), m_phaseId(0), m_ignorePhaseIdCheck(false)
{
    m_serverSideVisibility.SetValue(SERVERSIDE_VISIBILITY_GHOST, GHOST_VISIBILITY_ALIVE | GHOST_VISIBILITY_GHOST);
    m_serverSideVisibilityDetect.SetValue(SERVERSIDE_VISIBILITY_GHOST, GHOST_VISIBILITY_ALIVE);
    m_deleted = false;
}

void WorldObject::SetWorldObject(bool on)
{
    if (!IsInWorld())
        return;

    GetMap()->AddObjectToSwitchList(this, on);
}

bool WorldObject::IsWorldObject() const
{
    if (m_isWorldObject)
        return true;

    if (ToCreature() && ToCreature()->m_isTempWorldObject)
        return true;

    return false;
}

void WorldObject::setActive(bool on)
{
    if (m_isActive == on)
        return;

    if (GetTypeId() == TYPEID_PLAYER)
        return;

    m_isActive = on;

    if (!IsInWorld())
        return;

    Map* map = FindMap();
    if (!map)
        return;

    if (on)
    {
        if (GetTypeId() == TYPEID_UNIT)
            map->AddToActive(this->ToCreature());
        else if (GetTypeId() == TYPEID_DYNAMICOBJECT)
            map->AddToActive((DynamicObject*)this);
        else if (GetTypeId() == TYPEID_GAMEOBJECT)
            map->AddToActive((GameObject*)this);
    }
    else
    {
        if (GetTypeId() == TYPEID_UNIT)
            map->RemoveFromActive(this->ToCreature());
        else if (GetTypeId() == TYPEID_DYNAMICOBJECT)
            map->RemoveFromActive((DynamicObject*)this);
        else if (GetTypeId() == TYPEID_GAMEOBJECT)
            map->RemoveFromActive((GameObject*)this);
    }
}

void WorldObject::CleanupsBeforeDelete(bool /*finalCleanup*/)
{
    if (IsInWorld())
        RemoveFromWorld();

    _visibilityPlayerList.clear();
    _hideForGuid.clear();
}

void WorldObject::_Create(uint32 guidlow, HighGuid guidhigh, uint32 phaseMask)
{
    Object::_Create(guidlow, 0, guidhigh);
    m_phaseMask = phaseMask;
}

uint32 WorldObject::GetZoneId() const
{
    return GetBaseMap()->GetZoneId(m_positionX, m_positionY, m_positionZ);
}

uint32 WorldObject::GetAreaId() const
{
    return GetBaseMap()->GetAreaId(m_positionX, m_positionY, m_positionZ);
}

void WorldObject::GetZoneAndAreaId(uint32& zoneid, uint32& areaid) const
{
    GetBaseMap()->GetZoneAndAreaId(zoneid, areaid, m_positionX, m_positionY, m_positionZ);
}

InstanceScript* WorldObject::GetInstanceScript()
{
    Map* map = GetMap();
    return map->IsDungeon() ? ((InstanceMap*)map)->GetInstanceScript() : NULL;
}

float WorldObject::GetDistanceZ(const WorldObject* obj) const
{
    float dz = fabs(GetPositionZH() - obj->GetPositionZH());
    float sizefactor = GetObjectSize() + obj->GetObjectSize();
    float dist = dz - sizefactor;
    return (dist > 0 ? dist : 0);
}

bool WorldObject::_IsWithinDist(WorldObject const* obj, float dist2compare, bool is3D, bool size) const
{
    float sizefactor = size ? GetObjectSize() + obj->GetObjectSize() : 0.0f;
    float maxdist = dist2compare + sizefactor;

    if (m_transport && obj->GetTransport() &&  obj->GetTransport()->GetGUIDLow() == m_transport->GetGUIDLow())
    {
        float dtx = m_movementInfo.transportPosition.m_positionX - obj->m_movementInfo.transportPosition.m_positionX;
        float dty = m_movementInfo.transportPosition.m_positionY - obj->m_movementInfo.transportPosition.m_positionY;
        float disttsq = dtx * dtx + dty * dty;
        if (is3D)
        {
            float dtz = m_movementInfo.transportPosition.m_positionZ - obj->m_movementInfo.transportPosition.m_positionZ;
            disttsq += dtz * dtz;
        }
        return disttsq < (maxdist * maxdist);
    }

    float dx = GetPositionX() - obj->GetPositionX();
    float dy = GetPositionY() - obj->GetPositionY();
    float distsq = dx*dx + dy*dy;
    if (is3D)
    {
        float dz = GetPositionZH() - obj->GetPositionZH();
        distsq += dz*dz;
    }

    return distsq < maxdist * maxdist;
}

bool WorldObject::IsWithinLOSInMap(const WorldObject* obj) const
{
    if (!IsInMap(obj))
        return false;

    //Throne of the Four Wind, hack fix for Alakir
    if (GetMapId() == 754)
    {
        if (Creature const* victim = obj->ToCreature())
            if (victim->GetEntry() == 46753)
                return true;

        if (Creature const* attacker = ToCreature())
            if (attacker->GetEntry() == 46753)
                return true;
    }

    float ox, oy, oz;
    obj->GetPosition(ox, oy, oz);
    return IsWithinLOS(ox, oy, oz);
}

bool WorldObject::IsWithinLOS(float ox, float oy, float oz) const
{
    /*float x, y, z;
    GetPosition(x, y, z);
    VMAP::IVMapManager* vMapManager = VMAP::VMapFactory::createOrGetVMapManager();
    return vMapManager->isInLineOfSight(GetMapId(), x, y, z+2.0f, ox, oy, oz+2.0f);*/
    if (IsInWorld())
        return GetMap()->isInLineOfSight(GetPositionX(), GetPositionY(), GetPositionZH()+2.f, ox, oy, oz+2.f, GetPhaseMask());

    return true;
}

bool WorldObject::GetDistanceOrder(WorldObject const* obj1, WorldObject const* obj2, bool is3D /* = true */) const
{
    float dx1 = GetPositionX() - obj1->GetPositionX();
    float dy1 = GetPositionY() - obj1->GetPositionY();
    float distsq1 = dx1*dx1 + dy1*dy1;
    if (is3D)
    {
        float dz1 = GetPositionZH() - obj1->GetPositionZH();
        distsq1 += dz1*dz1;
    }

    float dx2 = GetPositionX() - obj2->GetPositionX();
    float dy2 = GetPositionY() - obj2->GetPositionY();
    float distsq2 = dx2*dx2 + dy2*dy2;
    if (is3D)
    {
        float dz2 = GetPositionZH() - obj2->GetPositionZH();
        distsq2 += dz2*dz2;
    }

    return distsq1 < distsq2;
}

bool WorldObject::IsInRange(WorldObject const* obj, float minRange, float maxRange, bool is3D /* = true */) const
{
    float dx = GetPositionX() - obj->GetPositionX();
    float dy = GetPositionY() - obj->GetPositionY();
    float distsq = dx*dx + dy*dy;
    if (is3D)
    {
        float dz = GetPositionZH() - obj->GetPositionZH();
        distsq += dz*dz;
    }

    float sizefactor = GetObjectSize() + obj->GetObjectSize();

    // check only for real range
    if (minRange > 0.0f)
    {
        float mindist = minRange + sizefactor;
        if (distsq < mindist * mindist)
            return false;
    }

    float maxdist = maxRange + sizefactor;
    return distsq < maxdist * maxdist;
}

bool WorldObject::IsInRange2d(float x, float y, float minRange, float maxRange) const
{
    float dx = GetPositionX() - x;
    float dy = GetPositionY() - y;
    float distsq = dx*dx + dy*dy;

    float sizefactor = GetObjectSize();

    // check only for real range
    if (minRange > 0.0f)
    {
        float mindist = minRange + sizefactor;
        if (distsq < mindist * mindist)
            return false;
    }

    float maxdist = maxRange + sizefactor;
    return distsq < maxdist * maxdist;
}

bool WorldObject::IsInRange3d(float x, float y, float z, float minRange, float maxRange) const
{
    float dx = GetPositionX() - x;
    float dy = GetPositionY() - y;
    float dz = GetPositionZH() - z;
    float distsq = dx*dx + dy*dy + dz*dz;

    float sizefactor = GetObjectSize();

    // check only for real range
    if (minRange > 0.0f)
    {
        float mindist = minRange + sizefactor;
        if (distsq < mindist * mindist)
            return false;
    }

    float maxdist = maxRange + sizefactor;
    return distsq < maxdist * maxdist;
}

void Position::RelocateOffset(const Position & offset)
{
    m_positionX = GetPositionX() + (offset.GetPositionX() * std::cos(GetOrientation()) + offset.GetPositionY() * std::sin(GetOrientation() + M_PI));
    m_positionY = GetPositionY() + (offset.GetPositionY() * std::cos(GetOrientation()) + offset.GetPositionX() * std::sin(GetOrientation()));
    m_positionZ = GetPositionZ() + offset.GetPositionZ();
    SetOrientation(GetOrientation() + offset.GetOrientation());
}

void Position::GetPositionOffsetTo(const Position & endPos, Position & retOffset) const
{
    float dx = endPos.GetPositionX() - GetPositionX();
    float dy = endPos.GetPositionY() - GetPositionY();

    retOffset.m_positionX = dx * std::cos(GetOrientation()) + dy * std::sin(GetOrientation());
    retOffset.m_positionY = dy * std::cos(GetOrientation()) - dx * std::sin(GetOrientation());
    retOffset.m_positionZ = endPos.GetPositionZ() - GetPositionZ();
    retOffset.SetOrientation(endPos.GetOrientation() - GetOrientation());
}

float Position::GetAngle(const Position* obj) const
{
    if (!obj)
        return 0;

    return GetAngle(obj->GetPositionX(), obj->GetPositionY());
}

// Return angle in range 0..2*pi
float Position::GetAngle(const float x, const float y) const
{
    float dx = x - GetPositionX();
    float dy = y - GetPositionY();

    float ang = atan2(dy, dx);
    ang = (ang >= 0) ? ang : 2 * M_PI + ang;
    return ang;
}

void Position::GetSinCos(const float x, const float y, float &vsin, float &vcos) const
{
    float dx = GetPositionX() - x;
    float dy = GetPositionY() - y;

    if (fabs(dx) < 0.001f && fabs(dy) < 0.001f)
    {
        float angle = (float)rand_norm()*static_cast<float>(2*M_PI);
        vcos = std::cos(angle);
        vsin = std::sin(angle);
    }
    else
    {
        float dist = sqrt((dx*dx) + (dy*dy));
        vcos = dx / dist;
        vsin = dy / dist;
    }
}

bool Position::HasInArc(float arc, const Position* obj) const
{
    // always have self in arc
    if (obj == this)
        return true;

    // move arc to range 0.. 2*pi
    arc = NormalizeOrientation(arc);

    float angle = GetAngle(obj);
    angle -= m_orientation;

    // move angle to range -pi ... +pi
    angle = NormalizeOrientation(angle);
    if (angle > M_PI)
        angle -= 2.0f*M_PI;

    float lborder = -1 * (arc/2.0f);                        // in range -pi..0
    float rborder = (arc/2.0f);                             // in range 0..pi
    return ((angle >= lborder) && (angle <= rborder));
}

bool WorldObject::IsInBetween(const Position* obj1, const Position* obj2, float size) const
{
    if (!obj1 || !obj2)
        return false;

    float dist = GetExactDist2d(obj1->GetPositionX(), obj1->GetPositionY());

    // not using sqrt() for performance
    if ((dist * dist) >= obj1->GetExactDist2dSq(obj2->GetPositionX(), obj2->GetPositionY()))
        return false;

    if (!size)
        size = GetObjectSize() / 2;

    float angle = obj1->GetAngle(obj2);

    // not using sqrt() for performance
    return (size * size) >= GetExactDist2dSq(obj1->GetPositionX() + cos(angle) * dist, obj1->GetPositionY() + sin(angle) * dist);
}

bool WorldObject::IsInBetweenShift(const Position* obj1, const Position* obj2, float size, float shift, float angleShift) const
{
    if (!obj1 || !obj2)
        return false;

    angleShift += obj1->GetOrientation();
    float destx = obj1->GetPositionX() + shift * std::cos(angleShift);
    float desty = obj1->GetPositionY() + shift * std::sin(angleShift);

    float dist = GetExactDist2d(destx, desty);

    // not using sqrt() for performance
    if ((dist * dist) >= obj1->GetExactDist2dSq(obj2->GetPositionX(), obj2->GetPositionY()))
        return false;

    if (!size)
        size = GetObjectSize() / 2;

    float angle = obj1->GetAngle(obj2);

    // not using sqrt() for performance
    return (size * size) >= GetExactDist2dSq(destx + cos(angle) * dist, desty + sin(angle) * dist);
}

bool WorldObject::IsInBetween(const WorldObject* obj1, float x2, float y2, float size) const
{
    if (!obj1)
        return false;

    float dist = GetExactDist2d(obj1->GetPositionX(), obj1->GetPositionY());

    // not using sqrt() for performance
    if ((dist * dist) >= obj1->GetExactDist2dSq(x2, y2))
        return false;

    if (!size)
        size = GetObjectSize() / 2;

    float angle = obj1->GetAngle(x2, y2);

    // not using sqrt() for performance
    return (size * size) >= GetExactDist2dSq(obj1->GetPositionX() + std::cos(angle) * dist, obj1->GetPositionY() + std::sin(angle) * dist);
}

bool WorldObject::isInFront(WorldObject const* target,  float arc) const
{
    return HasInArc(arc, target);
}

bool WorldObject::isInBack(WorldObject const* target, float arc) const
{
    return !HasInArc(2 * M_PI - arc, target);
}

void WorldObject::GetRandomPoint(const Position &pos, float distance, float &rand_x, float &rand_y, float &rand_z) const
{
    if (!distance)
    {
        pos.GetPosition(rand_x, rand_y, rand_z);
        return;
    }

    // angle to face `obj` to `this`
    float angle = (float)rand_norm()*static_cast<float>(2*M_PI);
    float new_dist = (float)rand_norm()*static_cast<float>(distance);

    rand_x = pos.m_positionX + new_dist * std::cos(angle);
    rand_y = pos.m_positionY + new_dist * std::sin(angle);
    rand_z = pos.m_positionZ;

    Trinity::NormalizeMapCoord(rand_x);
    Trinity::NormalizeMapCoord(rand_y);
    UpdateGroundPositionZ(rand_x, rand_y, rand_z);            // update to LOS height if available
}

void WorldObject::UpdateGroundPositionZ(float x, float y, float &z) const
{
    float new_z = GetBaseMap()->GetHeight(GetPhaseMask(), x, y, z, true);
    if (new_z > INVALID_HEIGHT)
        z = new_z+ 0.05f;                                   // just to be sure that we are not a few pixel under the surface
}

void WorldObject::UpdateAllowedPositionZ(float x, float y, float &z) const
{
    float _offset = GetPositionH() < 2.0f ? 2.0f : 0.0f; // For find correct position Z
    bool isFalling = m_movementInfo.HasMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);

    switch (GetTypeId())
    {
        case TYPEID_UNIT:
        {
            Unit* victim = ToCreature()->getVictim();
            if (victim)
            {
                // anyway creature move to victim for thinly Z distance (shun some VMAP wrong ground calculating)
                if (fabs(GetPositionZ() - victim->GetPositionZ()) < 5.0f)
                    return;
            }
            // non fly unit don't must be in air
            // non swim unit must be at ground (mostly speedup, because it don't must be in water and water level check less fast
            if (!ToCreature()->CanFly())
            {
                bool canSwim = ToCreature()->CanSwim();
                float ground_z = z;
                float max_z = canSwim
                    ? GetBaseMap()->GetWaterOrGroundLevel(x, y, z + _offset, &ground_z, !ToUnit()->HasAuraType(SPELL_AURA_WATER_WALK))
                    : ((ground_z = GetBaseMap()->GetHeight(GetPhaseMask(), x, y, z + _offset, true)));

                if (isFalling) // Allowed point in air if we falling
                    if ((z - max_z) > 2.0f)
                        return;

                max_z += GetPositionH();
                ground_z += GetPositionH();
                if (max_z > INVALID_HEIGHT)
                {
                    if (z > max_z && !IsInWater())
                        z = max_z;
                    else if (z < ground_z)
                        z = ground_z;
                }
            }
            else
            {
                float ground_z = GetBaseMap()->GetHeight(GetPhaseMask(), x, y, z + _offset, true);
                ground_z += GetPositionH();
                if (z < ground_z)
                    z = ground_z;
            }
            break;
        }
        case TYPEID_PLAYER:
        {
            // for server controlled moves playr work same as creature (but it can always swim)
            if (!ToPlayer()->CanFly())
            {
                float ground_z = z;
                float max_z = GetBaseMap()->GetWaterOrGroundLevel(x, y, z + _offset, &ground_z, !ToUnit()->HasAuraType(SPELL_AURA_WATER_WALK));
                max_z += GetPositionH();
                ground_z += GetPositionH();

                if (isFalling) // Allowed point in air if we falling
                    if ((z - max_z) > 2.0f)
                        return;

                if (max_z > INVALID_HEIGHT)
                {
                    if (z > max_z && !IsInWater())
                        z = max_z;
                    else if (z < ground_z)
                        z = ground_z;
                }
            }
            else
            {
                float ground_z = GetBaseMap()->GetHeight(GetPhaseMask(), x, y, z + _offset, true);
                ground_z += GetPositionH();
                if (z < ground_z)
                    z = ground_z;
            }
            break;
        }
        default:
        {
            float ground_z = GetBaseMap()->GetHeight(GetPhaseMask(), x, y, z + _offset, true);
            ground_z += GetPositionH();

            if (isFalling) // Allowed point in air if we falling
                if ((z - ground_z) > 2.0f)
                    return;

            if (ground_z > INVALID_HEIGHT)
                z = ground_z;
            break;
        }
    }
}

bool Position::IsPositionValid() const
{
    return Trinity::IsValidMapCoord(m_positionX, m_positionY, m_positionZ, m_orientation);
}

float WorldObject::CalcVisibilityRange(const WorldObject* obj) const
{
    if (isActiveObject() && !ToPlayer())
        return MAX_VISIBILITY_DISTANCE;
    else
        if (GetMap())
        {
            if (Player const* thisPlayer = ToPlayer())
                return GetMap()->GetVisibilityRange(thisPlayer->getCurrentUpdateZoneID(), thisPlayer->getCurrentUpdateAreaID());
            else if (Creature const* creature = ToCreature())
                return GetMap()->GetVisibilityRange(creature->getCurrentUpdateZoneID(), creature->getCurrentUpdateAreaID());
            else
                return GetMap()->GetVisibilityRange();
        }

    return MAX_VISIBILITY_DISTANCE;
}

float WorldObject::GetGridActivationRange() const
{
    if (Player const* thisPlayer = ToPlayer())
        return GetMap()->GetVisibilityRange(thisPlayer->getCurrentUpdateZoneID(), thisPlayer->getCurrentUpdateAreaID());
    else if (ToCreature())
        return ToCreature()->m_SightDistance;
    else
        return 0.0f;
}

float WorldObject::GetVisibilityCombatLog() const
{
    return SIGHT_RANGE_UNIT;
}

float WorldObject::GetSightRange(const WorldObject* target) const
{
    if (ToUnit())
    {
        if (Player const* thisPlayer = ToPlayer())
        {
            if (target && target->isActiveObject() && !target->ToPlayer())
                return MAX_VISIBILITY_DISTANCE;
            else
                return GetMap()->GetVisibilityRange(thisPlayer->getCurrentUpdateZoneID(), thisPlayer->getCurrentUpdateAreaID());
        }
        else if (ToCreature())
            return ToCreature()->m_SightDistance;
        else
            return SIGHT_RANGE_UNIT;
    }

    return 0.0f;
}

void WorldObject::SetVisible(bool x)
{
    if (!x)
        m_serverSideVisibility.SetValue(SERVERSIDE_VISIBILITY_GM, SEC_GAMEMASTER);
    else
        m_serverSideVisibility.SetValue(SERVERSIDE_VISIBILITY_GM, SEC_PLAYER);

    UpdateObjectVisibility();
}

bool WorldObject::canSeeOrDetect(WorldObject const* obj, bool ignoreStealth, bool distanceCheck) const
{
    if (this == obj)
        return true;

    if (IS_PLAYER_GUID(GetGUID()))
    {
        if (obj->MustBeVisibleOnlyForSomePlayers() && IS_PLAYER_GUID(GetGUID()))
        {
            Player const* thisPlayer = ToPlayer();

            if (!thisPlayer)
                return false;

            Group const* group = thisPlayer->GetGroup();

            if (!obj->IsPlayerInPersonnalVisibilityList(thisPlayer->GetGUID()) &&
                (!group || !obj->IsGroupInPersonnalVisibilityList(group->GetGUID())))
                return false;
        }

        if (obj->HideForSomePlayers() && obj->ShouldHideFor(GetGUID()))
            return false;
    }

    if (IS_PLAYER_GUID(GetGUID()) && IS_GAMEOBJECT_GUID(obj->GetGUID()))
    {
        Player const* thisPlayer = ToPlayer();
        if (thisPlayer && thisPlayer->IsPlayerLootCooldown(obj->GetEntry()))
            return false;
    }

    if (obj->IsNeverVisible() || CanNeverSee(obj))
        return false;

    if (obj->IsAlwaysVisibleFor(this) || CanAlwaysSee(obj))
        return true;

    bool corpseVisibility = false;
    if (distanceCheck)
    {
        bool corpseCheck = false;
        bool onArena = false;   //on arena we have always see all

        if (Player const* thisPlayer = ToPlayer())
        {
            if (thisPlayer->HaveExtraLook(obj->GetGUID()))
                return true;

            //not see befor enter vehicle.
            if (Creature const* creature = obj->ToCreature())
                if (creature->onVehicleAccessoryInit())
                    return false;

            onArena = thisPlayer->InArena();

            if (thisPlayer->isDead() && thisPlayer->GetHealth() > 0 && // Cheap way to check for ghost state
                !(obj->m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GHOST) & m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GHOST) & GHOST_VISIBILITY_GHOST))
            {
                if (Corpse* corpse = thisPlayer->GetCorpse())
                {
                    corpseCheck = true;
                    if (corpse->IsWithinDist(thisPlayer, GetSightRange(obj), false))
                        if (corpse->IsWithinDist(obj, GetSightRange(obj), false))
                            corpseVisibility = true;
                }
            }
        }

        WorldObject const* viewpoint = this;
        if (Player const* player = ToPlayer())
            viewpoint = player->GetViewpoint();

        if (!viewpoint)
            viewpoint = this;

        if (!corpseCheck && !onArena && !viewpoint->IsWithinDist(obj, GetSightRange(obj), false))
            return false;
    }

    // GM visibility off or hidden NPC
    if (!obj->m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GM))
    {
        // Stop checking other things for GMs
        if (m_serverSideVisibilityDetect.GetValue(SERVERSIDE_VISIBILITY_GM))
            return true;
    }
    else
        return m_serverSideVisibilityDetect.GetValue(SERVERSIDE_VISIBILITY_GM) >= obj->m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GM);

    // Ghost players, Spirit Healers, and some other NPCs
    if (!corpseVisibility && !(obj->m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GHOST) & m_serverSideVisibilityDetect.GetValue(SERVERSIDE_VISIBILITY_GHOST)))
    {
        // Alive players can see dead players in some cases, but other objects can't do that
        if (Player const* thisPlayer = ToPlayer())
        {
            if (Player const* objPlayer = obj->ToPlayer())
            {
                if (thisPlayer->GetTeam() != objPlayer->GetTeam() || !thisPlayer->IsGroupVisibleFor(objPlayer))
                    return false;
            }
            else
                return false;
        }
        else
            return false;
    }

    if (obj->IsInvisibleDueToDespawn())
        return false;

    if (!CanDetect(obj, ignoreStealth))
        return false;

    return true;
}

bool WorldObject::CanDetect(WorldObject const* obj, bool ignoreStealth) const
{
    const WorldObject* seer = this;

    // Pets don't have detection, they use the detection of their masters
    if (const Unit* thisUnit = ToUnit())
        if (Unit* controller = thisUnit->GetCharmerOrOwner())
            seer = controller;

    if (obj->IsAlwaysDetectableFor(seer))
        return true;

    if (!ignoreStealth && !seer->CanDetectInvisibilityOf(obj))
        return false;

    if (!ignoreStealth && !seer->CanDetectStealthOf(obj))
        return false;

    return true;
}

bool WorldObject::CanDetectInvisibilityOf(WorldObject const* obj) const
{
    uint32 mask = obj->m_invisibility.GetFlags() & m_invisibilityDetect.GetFlags();

    // Check for not detected types
    if (mask != obj->m_invisibility.GetFlags())
        return false;

    if (obj->ToUnit())
        if ((m_invisibility.GetFlags() & obj->m_invisibilityDetect.GetFlags()) != m_invisibility.GetFlags())
        {
            if (obj->m_invisibility.GetFlags() != 0 || !isType(TYPEMASK_UNIT) || !ToUnit()->HasAuraType(SPELL_AURA_SEE_WHILE_INVISIBLE))
                return false;
        }

    for (uint32 i = 0; i < TOTAL_INVISIBILITY_TYPES; ++i)
    {
        if (!(mask & (1 << i)))
            continue;

        int32 objInvisibilityValue = obj->m_invisibility.GetValue(InvisibilityType(i));
        int32 ownInvisibilityDetectValue = m_invisibilityDetect.GetValue(InvisibilityType(i));

        // Too low value to detect
        if (ownInvisibilityDetectValue < objInvisibilityValue)
            return false;
    }

    return true;
}

bool WorldObject::CanDetectStealthOf(WorldObject const* obj) const
{
    // Combat reach is the minimal distance (both in front and behind),
    //   and it is also used in the range calculation.
    // One stealth point increases the visibility range by 0.3 yard.

    if (!obj->m_stealth.GetFlags())
        return true;

    //!Hack fix - Subterfuge(Rogue)
    if (Player const* plr = obj->ToPlayer())
        if (plr->HasAura(115192))
            return true;

    float distance = GetExactDist(obj);
    float combatReach = 0.0f;

    if (isType(TYPEMASK_UNIT))
        combatReach = ((Unit*)this)->GetCombatReach();

//     if (distance < combatReach)
//         return true;
// 
//     if (Player const* player = ToPlayer())
//         if(player->HaveAtClient(obj) && distance < (ATTACK_DISTANCE * 2))
//             return true;

    if (!HasInArc(M_PI, obj))
        return false;

    GameObject const* go = ToGameObject();
    for (uint32 i = 0; i < TOTAL_STEALTH_TYPES; ++i)
    {
        if (!(obj->m_stealth.GetFlags() & (1 << i)))
            continue;

        if (isType(TYPEMASK_UNIT))
            if (((Unit*)this)->HasAuraTypeWithMiscvalue(SPELL_AURA_DETECT_STEALTH, i))
                return true;

        // Starting points
        int32 detectionValue = 30;

        // Level difference: 5 point / level, starting from level 1.
        // There may be spells for this and the starting points too, but
        // not in the DBCs of the client.
        detectionValue += int32(getLevelForTarget(obj) - 1) * 5;

        // Apply modifiers
        detectionValue += m_stealthDetect.GetValue(StealthType(i));
        if (go)
            if (Unit* owner = go->GetOwner())
                detectionValue -= int32(owner->getLevelForTarget(this) - 1) * 5;

        detectionValue -= obj->m_stealth.GetValue(StealthType(i));

        // Calculate max distance
        float visibilityRange = float(detectionValue) * 0.3f + combatReach;

        if (visibilityRange > MAX_PLAYER_STEALTH_DETECT_RANGE)
            visibilityRange = MAX_PLAYER_STEALTH_DETECT_RANGE;

        if (distance > visibilityRange)
            return false;
    }

    return true;
}

bool WorldObject::IsPlayerInPersonnalVisibilityList(uint64 guid) const
{
    if (!IS_PLAYER_GUID(guid))
        return false;

    return _visibilityPlayerList.find(guid) != _visibilityPlayerList.end();
}

bool WorldObject::IsGroupInPersonnalVisibilityList(uint64 guid) const
{
    if (!IS_GROUP(guid))
        return false;

    return _visibilityPlayerList.find(guid) != _visibilityPlayerList.end();
}

void WorldObject::AddPlayersInPersonnalVisibilityList(std::list<uint64> viewerList)
{
    for (std::list<uint64>::const_iterator guid = viewerList.begin(); guid != viewerList.end(); ++guid)
    {
        if (!IS_PLAYER_GUID(*guid))
            continue;

        _visibilityPlayerList.insert(*guid);
    }
}

void WorldObject::SendPlaySound(uint32 Sound, bool OnlySelf)
{
    ObjectGuid source = GetGUID();

    WorldPacket data(SMSG_PLAY_SOUND, 12);
    data.WriteGuidMask<0, 2, 4, 7, 6, 5, 1, 3>(source);
    data.WriteGuidBytes<3, 4, 2, 6, 1, 5, 0>(source);
    data << uint32(Sound);
    data.WriteGuidBytes<7>(source);

    if (OnlySelf && GetTypeId() == TYPEID_PLAYER)
        this->ToPlayer()->GetSession()->SendPacket(&data);
    else
        SendMessageToSet(&data, true); // ToSelf ignored in this case
}

void Object::ForceValuesUpdateAtIndex(uint32 i)
{
    _changedFields[i] = true;
    if (m_inWorld == 1 && !m_objectUpdated)
    {
        sObjectAccessor->AddUpdateObject(this);
        m_objectUpdated = true;
    }
}

namespace Trinity
{
    class MonsterChatBuilder
    {
        public:
            MonsterChatBuilder(WorldObject const& obj, ChatMsg msgtype, int32 textId, uint32 language, uint64 targetGUID)
                : i_object(obj), i_msgtype(msgtype), i_textId(textId), i_language(language), i_targetGUID(targetGUID) {}
            void operator()(WorldPacket& data, LocaleConstant loc_idx)
            {
                char const* text = sObjectMgr->GetTrinityString(i_textId, loc_idx);

                // TODO: i_object.GetName() also must be localized?
                i_object.BuildMonsterChat(&data, i_msgtype, text, i_language, i_object.GetNameForLocaleIdx(loc_idx), i_targetGUID);
            }

        private:
            WorldObject const& i_object;
            ChatMsg i_msgtype;
            int32 i_textId;
            uint32 i_language;
            uint64 i_targetGUID;
    };

    class MonsterCustomChatBuilder
    {
        public:
            MonsterCustomChatBuilder(WorldObject const& obj, ChatMsg msgtype, const char* text, uint32 language, uint64 targetGUID)
                : i_object(obj), i_msgtype(msgtype), i_text(text), i_language(language), i_targetGUID(targetGUID) {}
            void operator()(WorldPacket& data, LocaleConstant loc_idx)
            {
                // TODO: i_object.GetName() also must be localized?
                i_object.BuildMonsterChat(&data, i_msgtype, i_text, i_language, i_object.GetNameForLocaleIdx(loc_idx), i_targetGUID);
            }

        private:
            WorldObject const& i_object;
            ChatMsg i_msgtype;
            const char* i_text;
            uint32 i_language;
            uint64 i_targetGUID;
    };
}                                                           // namespace Trinity

void WorldObject::MonsterSay(const char* text, uint32 language, uint64 TargetGuid)
{
    CellCoord p = Trinity::ComputeCellCoord(GetPositionX(), GetPositionY());

    Cell cell(p);
    cell.SetNoCreate();

    Trinity::MonsterCustomChatBuilder say_build(*this, CHAT_MSG_MONSTER_SAY, text, language, TargetGuid);
    Trinity::LocalizedPacketDo<Trinity::MonsterCustomChatBuilder> say_do(say_build);
    Trinity::PlayerDistWorker<Trinity::LocalizedPacketDo<Trinity::MonsterCustomChatBuilder> > say_worker(this, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY), say_do);

    cell.Visit(p, Trinity::makeWorldVisitor(say_worker), *GetMap(), *this, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY));
}

void WorldObject::MonsterSay(int32 textId, uint32 language, uint64 TargetGuid)
{
    CellCoord p = Trinity::ComputeCellCoord(GetPositionX(), GetPositionY());

    Cell cell(p);
    cell.SetNoCreate();

    Trinity::MonsterChatBuilder say_build(*this, CHAT_MSG_MONSTER_SAY, textId, language, TargetGuid);
    Trinity::LocalizedPacketDo<Trinity::MonsterChatBuilder> say_do(say_build);
    Trinity::PlayerDistWorker<Trinity::LocalizedPacketDo<Trinity::MonsterChatBuilder> > say_worker(this, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY), say_do);

    cell.Visit(p, Trinity::makeWorldVisitor(say_worker), *GetMap(), *this, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY));
}

void WorldObject::MonsterYell(const char* text, uint32 language, uint64 TargetGuid)
{
    CellCoord p = Trinity::ComputeCellCoord(GetPositionX(), GetPositionY());

    Cell cell(p);
    cell.SetNoCreate();

    Trinity::MonsterCustomChatBuilder say_build(*this, CHAT_MSG_MONSTER_YELL, text, language, TargetGuid);
    Trinity::LocalizedPacketDo<Trinity::MonsterCustomChatBuilder> say_do(say_build);
    Trinity::PlayerDistWorker<Trinity::LocalizedPacketDo<Trinity::MonsterCustomChatBuilder> > say_worker(this, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_YELL), say_do);

    cell.Visit(p, Trinity::makeWorldVisitor(say_worker), *GetMap(), *this, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_YELL));
}

void WorldObject::MonsterYell(int32 textId, uint32 language, uint64 TargetGuid)
{
    CellCoord p = Trinity::ComputeCellCoord(GetPositionX(), GetPositionY());

    Cell cell(p);
    cell.SetNoCreate();

    Trinity::MonsterChatBuilder say_build(*this, CHAT_MSG_MONSTER_YELL, textId, language, TargetGuid);
    Trinity::LocalizedPacketDo<Trinity::MonsterChatBuilder> say_do(say_build);
    Trinity::PlayerDistWorker<Trinity::LocalizedPacketDo<Trinity::MonsterChatBuilder> > say_worker(this, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_YELL), say_do);

    cell.Visit(p, Trinity::makeWorldVisitor(say_worker), *GetMap(), *this, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_YELL));
}

void WorldObject::MonsterYellToZone(int32 textId, uint32 language, uint64 TargetGuid)
{
    Trinity::MonsterChatBuilder say_build(*this, CHAT_MSG_MONSTER_YELL, textId, language, TargetGuid);
    Trinity::LocalizedPacketDo<Trinity::MonsterChatBuilder> say_do(say_build);

    uint32 zoneid = GetCurrentZoneId();

    Map::PlayerList const& pList = GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = pList.begin(); itr != pList.end(); ++itr)
        if (itr->getSource()->getCurrentUpdateZoneID() == zoneid)
            say_do(itr->getSource());
}

void WorldObject::MonsterTextEmote(const char* text, uint64 TargetGuid, bool IsBossEmote)
{
    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data, IsBossEmote ? CHAT_MSG_RAID_BOSS_EMOTE : CHAT_MSG_MONSTER_EMOTE, text, LANG_UNIVERSAL, GetName(), TargetGuid);
    SendMessageToSetInRange(&data, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE), true);
}

void WorldObject::MonsterTextEmote(int32 textId, uint64 TargetGuid, bool IsBossEmote)
{
    CellCoord p = Trinity::ComputeCellCoord(GetPositionX(), GetPositionY());

    Cell cell(p);
    cell.SetNoCreate();

    Trinity::MonsterChatBuilder say_build(*this, IsBossEmote ? CHAT_MSG_RAID_BOSS_EMOTE : CHAT_MSG_MONSTER_EMOTE, textId, LANG_UNIVERSAL, TargetGuid);
    Trinity::LocalizedPacketDo<Trinity::MonsterChatBuilder> say_do(say_build);
    Trinity::PlayerDistWorker<Trinity::LocalizedPacketDo<Trinity::MonsterChatBuilder> > say_worker(this, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE), say_do);

    cell.Visit(p, Trinity::makeWorldVisitor(say_worker), *GetMap(), *this, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE));
}

void WorldObject::MonsterWhisper(const char* text, uint64 receiver, bool IsBossWhisper)
{
    Player* player = ObjectAccessor::FindPlayer(receiver);
    if (!player || !player->GetSession())
        return;

    LocaleConstant loc_idx = player->GetSession()->GetSessionDbLocaleIndex();

    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data, IsBossWhisper ? CHAT_MSG_RAID_BOSS_WHISPER : CHAT_MSG_MONSTER_WHISPER, text, LANG_UNIVERSAL, GetNameForLocaleIdx(loc_idx), receiver);

    player->GetSession()->SendPacket(&data);
}

void WorldObject::MonsterWhisper(int32 textId, uint64 receiver, bool IsBossWhisper)
{
    Player* player = ObjectAccessor::FindPlayer(receiver);
    if (!player || !player->GetSession())
        return;

    LocaleConstant loc_idx = player->GetSession()->GetSessionDbLocaleIndex();
    char const* text = sObjectMgr->GetTrinityString(textId, loc_idx);

    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data, IsBossWhisper ? CHAT_MSG_RAID_BOSS_WHISPER : CHAT_MSG_MONSTER_WHISPER, text, LANG_UNIVERSAL, GetNameForLocaleIdx(loc_idx), receiver);

    player->GetSession()->SendPacket(&data);
}

void WorldObject::BuildMonsterChat(WorldPacket* data, uint8 msgtype, char const* text, uint32 language, char const* name, uint64 targetGuid) const
{
    Trinity::ChatData c;
    c.sourceGuid = GetGUID();
    c.targetGuid = targetGuid;
    c.message = text;
    c.sourceName = name;
    c.language = language;
    c.chatType = msgtype;

    Trinity::BuildChatPacket(*data, c);
}

void WorldObject::SendMessageToSet(WorldPacket* data, bool self)
{
    if (IsInWorld())
        SendMessageToSetInRange(data, CalcVisibilityRange(), self);
}

void WorldObject::SendMessageToSetInRange(WorldPacket* data, float dist, bool /*self*/)
{
    Trinity::MessageDistDeliverer notifier(this, data, dist);
    Trinity::VisitNearbyWorldObject(this, dist, notifier);
}

void WorldObject::SendMessageToSet(WorldPacket* data, Player const* skipped_rcvr)
{
    Trinity::MessageDistDeliverer notifier(this, data, CalcVisibilityRange(), false, skipped_rcvr);
    Trinity::VisitNearbyWorldObject(this, CalcVisibilityRange(), notifier);
}

void WorldObject::SendObjectDeSpawnAnim(uint64 guid)
{
    WorldPacket data(SMSG_GAMEOBJECT_DESPAWN_ANIM, 8 + 1);
    data.WriteGuidMask<6, 5, 0, 1, 3, 2, 7, 4>(guid);
    data.WriteGuidBytes<2, 4, 1, 3, 0, 7, 5, 6>(guid);
    SendMessageToSet(&data, true);
}

void WorldObject::SetMap(Map* map)
{
    ASSERT(map);
    ASSERT(!IsInWorld() || GetTypeId() == TYPEID_CORPSE);
    if (m_currMap == map) // command add npc: first create, than loadfromdb
        return;
    if (m_currMap)
    {
        TC_LOG_FATAL("server", "WorldObject::SetMap: obj %u new map %u %u, old map %u %u", (uint32)GetTypeId(), map->GetId(), map->GetInstanceId(), m_currMap->GetId(), m_currMap->GetInstanceId());
        ASSERT(false);
    }
    m_currMap = map;
    m_mapId = map->GetId();
    m_InstanceId = map->GetInstanceId();
    if (IsWorldObject())
        m_currMap->AddWorldObject(this);
}

void WorldObject::ResetMap()
{
    ASSERT(m_currMap);
    ASSERT(!IsInWorld());
    if (IsWorldObject())
        m_currMap->RemoveWorldObject(this);
    m_currMap = NULL;
    //maybe not for corpse
    //m_mapId = 0;
    //m_InstanceId = 0;
}

Map const* WorldObject::GetBaseMap() const
{
    ASSERT(m_currMap);
    return m_currMap->GetParent();
}

void WorldObject::AddObjectToRemoveList()
{
    ASSERT(m_uint32Values);

    Map* map = FindMap();
    if (!map)
    {
        TC_LOG_ERROR("server", "Object (TypeId: %u Entry: %u GUID: %u) at attempt add to move list not have valid map (Id: %u).", GetTypeId(), GetEntry(), GetGUIDLow(), GetMapId());
        return;
    }

    map->AddObjectToRemoveList(this);
}

TempSummon* Map::SummonCreature(uint32 entry, Position const& pos, SummonPropertiesEntry const* properties /*= NULL*/, uint32 duration /*= 0*/, Unit* summoner /*= NULL*/, uint64 targetGuid /*= 0*/, uint32 spellId /*= 0*/, int32 vehId /*= 0*/, uint64 viewerGuid /*= 0*/, std::list<uint64>* viewersList /*= NULL*/)
{
    if(summoner)
    {
        std::list<Creature*> creatures;
        summoner->GetAliveCreatureListWithEntryInGrid(creatures, entry, 110.0f);
        if(creatures.size() > 50)
            return NULL;
    }

    uint32 mask = UNIT_MASK_SUMMON;
    if (properties)
    {
        switch (properties->Category)
        {
            case SUMMON_CATEGORY_PET:
                mask = UNIT_MASK_GUARDIAN;
                break;
            case SUMMON_CATEGORY_PUPPET:
                mask = UNIT_MASK_PUPPET;
                break;
            case SUMMON_CATEGORY_VEHICLE:
                if (properties->Id == 3384) //hardfix despawn npc 63872
                    mask = UNIT_MASK_SUMMON;
                else
                    mask = UNIT_MASK_MINION;
                break;
            case SUMMON_CATEGORY_WILD:
            case SUMMON_CATEGORY_ALLY:
            case SUMMON_CATEGORY_UNK:
            {
                switch (properties->Type)
                {
                    case SUMMON_TYPE_MINION:
                    case SUMMON_TYPE_GUARDIAN:
                    case SUMMON_TYPE_GUARDIAN2:
                    case SUMMON_TYPE_OBJECT:
                        if (properties->Id == 1141)
                            mask = UNIT_MASK_TOTEM;
                        else
                            mask = UNIT_MASK_GUARDIAN;
                        break;
                    case SUMMON_TYPE_TOTEM:
                    case SUMMON_TYPE_BANNER:
                    case SUMMON_TYPE_STATUE:
                        mask = UNIT_MASK_TOTEM;
                        break;
                    case SUMMON_TYPE_VEHICLE:
                    case SUMMON_TYPE_VEHICLE2:
                    case SUMMON_TYPE_GATE:
                        mask = UNIT_MASK_SUMMON;
                        break;
                    case SUMMON_TYPE_MINIPET:
                        mask = UNIT_MASK_MINION;
                        break;
                    default:
                    {
                        if (properties->Flags & 512 ||
                            properties->Id == 2921 ||
                            properties->Id == 3459 ||
                            properties->Id == 3097) // Mirror Image, Summon Gargoyle
                            mask = UNIT_MASK_GUARDIAN;
                            break;
                    }
                }
                break;
            }
            default:
                return NULL;
        }
    }

    uint32 phase = PHASEMASK_NORMAL;
    uint32 team = 0;
    if (summoner)
    {
        phase = summoner->GetPhaseMask();
        if (summoner->GetTypeId() == TYPEID_PLAYER)
            team = summoner->ToPlayer()->GetTeam();
    }

    TempSummon* summon = NULL;
    switch (mask)
    {
        case UNIT_MASK_SUMMON:
            summon = new TempSummon(properties, summoner, false);
            break;
        case UNIT_MASK_GUARDIAN:
            summon = new Guardian(properties, summoner, false);
            break;
        case UNIT_MASK_PUPPET:
            summon = new Puppet(properties, summoner);
            break;
        case UNIT_MASK_TOTEM:
            summon = new Totem(properties, summoner);
            break;
        case UNIT_MASK_MINION:
            summon = new Minion(properties, summoner, false);
            break;
        default:
            return NULL;
    }

    if (!summon->Create(sObjectMgr->GenerateLowGuid(HIGHGUID_UNIT), this, phase, entry, vehId, team, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), pos.GetOrientation()))
    {
        delete summon;
        return NULL;
    }

    summon->SetUInt32Value(UNIT_CREATED_BY_SPELL, spellId);
    if (summoner)
        summon->SetUInt32Value(UNIT_FIELD_DEMON_CREATOR, summoner->GetGUID());

    summon->SetTargetGUID(targetGuid);

    summon->SetHomePosition(pos);

    summon->InitStats(duration);

    if (viewerGuid)
        summon->AddPlayerInPersonnalVisibilityList(viewerGuid);

    if (viewersList)
        summon->AddPlayersInPersonnalVisibilityList(*viewersList);

    AddToMap(summon->ToCreature());
    summon->InitSummon();
    summon->CastPetAuras(true);

    //TC_LOG_DEBUG("pets", "Map::SummonCreature summoner %u entry %i mask %i", summoner ? summoner->GetGUID() : 0, entry, mask);

    //ObjectAccessor::UpdateObjectVisibility(summon);

    return summon;
}

void WorldObject::SetZoneScript()
{
    if (Map* map = FindMap())
    {
        if (map->IsDungeon())
            m_zoneScript = (ZoneScript*)((InstanceMap*)map)->GetInstanceScript();
        else if (!map->IsBattlegroundOrArena())
        {
            if (Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(GetCurrentZoneId()))
                m_zoneScript = bf;
            else
            {
                if (Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(GetCurrentZoneId()))
                    m_zoneScript = bf;
                else
                    m_zoneScript = sOutdoorPvPMgr->GetZoneScript(GetCurrentZoneId());
            }
        }
    }
}

TempSummon* WorldObject::SummonCreature(uint32 entry, const Position &pos, uint64 targetGuid, TempSummonType spwtype, uint32 duration, uint32 spellId /*= 0*/, SummonPropertiesEntry const* properties /*= NULL*/) const
{
    if (Map* map = FindMap())
    {
        if(!ToUnit())
        {
            std::list<Creature*> creatures;
            GetAliveCreatureListWithEntryInGrid(creatures, entry, 110.0f);
            if(creatures.size() > 50)
                return NULL;
        }
        if (TempSummon* summon = map->SummonCreature(entry, pos, properties, duration, isType(TYPEMASK_UNIT) ? (Unit*)this : NULL, targetGuid, spellId))
        {
            summon->SetTempSummonType(spwtype);
            return summon;
        }
    }

    return NULL;
}

TempSummon* WorldObject::SummonCreature(uint32 entry, const Position &pos, TempSummonType spwtype, uint32 duration, int32 vehId, uint64 viewerGuid, std::list<uint64>* viewersList) const
{
    if (Map* map = FindMap())
    {
        if(!ToUnit())
        {
            std::list<Creature*> creatures;
            GetAliveCreatureListWithEntryInGrid(creatures, entry, 110.0f);
            if(creatures.size() > 50)
                return NULL;
        }
        if (TempSummon* summon = map->SummonCreature(entry, pos, NULL, duration, isType(TYPEMASK_UNIT) ? (Unit*)this : NULL, 0, 0, vehId, viewerGuid, viewersList))
        {
            summon->SetTempSummonType(spwtype);
            return summon;
        }
    }

    return NULL;
}

Pet* Player::SummonPet(uint32 entry, float x, float y, float z, float ang, PetType petType, uint32 duration, uint32 spellId, bool stampeded)
{
    if (getClass() == CLASS_HUNTER)
        petType = HUNTER_PET;

    Pet* pet = new Pet(this, petType);

    pet->Relocate(x, y, z, ang);

    //summoned pets always non-curent!
    if (petType == SUMMON_PET || petType == HUNTER_PET)
    {
        // This check done in LoadPetFromDB, but we should not continue this function if pet not alowed
        if (!CanSummonPet(entry))
        {
            delete pet;
            return NULL;
        }

        if (pet->LoadPetFromDB(this, entry, 0, stampeded))
        {
            if (duration > 0)
                pet->SetDuration(duration);

            return pet;
        }
    }

    // petentry == 0 for hunter "call pet" (current pet summoned if any)
    if (!entry)
    {
        TC_LOG_ERROR("pets", "no such entry %u", entry);
        delete pet;
        return NULL;
    }

    if (!pet->IsPositionValid())
    {
        TC_LOG_ERROR("pets", "Pet (guidlow %d, entry %d) not summoned. Suggested coordinates isn't valid (X: %f Y: %f)", pet->GetGUIDLow(), pet->GetEntry(), pet->GetPositionX(), pet->GetPositionY());
        delete pet;
        return NULL;
    }

    Map* map = GetMap();
    uint32 pet_number = sObjectMgr->GeneratePetNumber();
    if (!pet->Create(sObjectMgr->GenerateLowGuid(HIGHGUID_PET), map, GetPhaseMask(), entry, pet_number))
    {
        TC_LOG_ERROR("pets", "no such creature entry %u", entry);
        delete pet;
        return NULL;
    }

    pet->SetCreatorGUID(GetGUID());
    pet->SetUInt32Value(UNIT_FIELD_FACTIONTEMPLATE, getFaction());
    pet->SetUInt32Value(UNIT_NPC_FLAGS, 0);
    pet->SetUInt32Value(UNIT_FIELD_BYTES_1, 0);
    pet->SetUInt32Value(UNIT_CREATED_BY_SPELL, spellId);

    if(petType == SUMMON_PET)
        pet->GetCharmInfo()->SetPetNumber(pet_number, true);

    // After SetPetNumber
    SetMinion(pet, true, stampeded);

    map->AddToMap(pet->ToCreature());

    pet->InitStatsForLevel(getLevel());

    if(petType == SUMMON_PET)
    {
        pet->SynchronizeLevelWithOwner();
        pet->InitPetCreateSpells();
        pet->SavePetToDB();
        PetSpellInitialize();
        SendTalentsInfoData(true);
    }

    if (getClass() == CLASS_WARLOCK)
        if (HasAura(108503))
            RemoveAura(108503);

    if (duration > 0)
        pet->SetDuration(duration);

    //TC_LOG_DEBUG("spell", "SummonPet entry %i, petType %i, spellId %i", entry, petType, spellId);

    pet->CastPetAuras(true);
    return pet;
}

GameObject* WorldObject::SummonGameObject(uint32 entry, float x, float y, float z, float ang, float rotation0, float rotation1, float rotation2, float rotation3, uint32 respawnTime, uint64 viewerGuid, std::list<uint64>* viewersList)
{
    if (!IsInWorld())
        return NULL;

    GameObjectTemplate const* goinfo = sObjectMgr->GetGameObjectTemplate(entry);
    if (!goinfo)
    {
        TC_LOG_ERROR("sql", "Gameobject template %u not found in database!", entry);
        return NULL;
    }
    Map* map = GetMap();
    GameObject* go = new GameObject();
    if (!go->Create(sObjectMgr->GenerateLowGuid(HIGHGUID_GAMEOBJECT), entry, map, GetPhaseMask(), x, y, z, ang, rotation0, rotation1, rotation2, rotation3, 100, GO_STATE_READY))
    {
        delete go;
        return NULL;
    }
    go->SetRespawnTime(respawnTime);

    // If we summon go by creature at despown - we will see deleted go.
    // If we summon go by creature with ownership in some cases we couldn't use it
    if (GetTypeId() == TYPEID_PLAYER || (GetTypeId() == TYPEID_UNIT && !respawnTime)) //not sure how to handle this
        ToUnit()->AddGameObject(go);
    else
        go->SetSpawnedByDefault(false);

    if (viewerGuid)
        go->AddPlayerInPersonnalVisibilityList(viewerGuid);

    if (viewersList)
        go->AddPlayersInPersonnalVisibilityList(*viewersList);

    map->AddToMap(go);

    return go;
}

Creature* WorldObject::SummonTrigger(float x, float y, float z, float ang, uint32 duration, CreatureAI* (*GetAI)(Creature*))
{
    TempSummonType summonType = (duration == 0) ? TEMPSUMMON_DEAD_DESPAWN : TEMPSUMMON_TIMED_DESPAWN;
    Creature* summon = SummonCreature(WORLD_TRIGGER, x, y, z, ang, summonType, duration);
    if (!summon)
        return NULL;

    //summon->SetName(GetName());
    if (GetTypeId() == TYPEID_PLAYER || GetTypeId() == TYPEID_UNIT)
    {
        summon->setFaction(((Unit*)this)->getFaction());
        summon->SetLevel(((Unit*)this)->getLevel());
    }

    if (GetAI)
        summon->AIM_Initialize(GetAI(summon));
    return summon;
}

void WorldObject::GetAttackableUnitListInRange(std::list<Unit*> &list, float fMaxSearchRange, bool size) const
{
    CellCoord p(Trinity::ComputeCellCoord(GetPositionX(), GetPositionY()));
    Cell cell(p);
    cell.SetNoCreate();

    Trinity::AnyUnitInObjectRangeCheck u_check(this, fMaxSearchRange, size);
    Trinity::UnitListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(this, list, u_check);

    cell.Visit(p, Trinity::makeWorldVisitor(searcher), *GetMap(), *this, fMaxSearchRange);
    cell.Visit(p, Trinity::makeGridVisitor(searcher), *GetMap(), *this, fMaxSearchRange);
}

void WorldObject::GetAreaTriggersWithEntryInRange(std::list<AreaTrigger*>& list, uint32 entry, uint64 casterGuid, float fMaxSearchRange) const
{
    Trinity::AreaTriggerWithEntryInObjectRangeCheck checker(this, entry, casterGuid, fMaxSearchRange);
    Trinity::AreaTriggerListSearcher<Trinity::AreaTriggerWithEntryInObjectRangeCheck> searcher(this, list, checker);
    Trinity::VisitNearbyObject(this, fMaxSearchRange, searcher);
}

Creature* WorldObject::FindNearestCreature(uint32 entry, float range, bool alive) const
{
    Creature* creature = NULL;
    Trinity::NearestCreatureEntryWithLiveStateInObjectRangeCheck checker(*this, entry, alive, range);
    Trinity::CreatureLastSearcher<Trinity::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(this, creature, checker);
    Trinity::VisitNearbyObject(this, range, searcher);
    return creature;
}

GameObject* WorldObject::FindNearestGameObject(uint32 entry, float range) const
{
    GameObject* go = NULL;
    Trinity::NearestGameObjectEntryInObjectRangeCheck checker(*this, entry, range);
    Trinity::GameObjectLastSearcher<Trinity::NearestGameObjectEntryInObjectRangeCheck> searcher(this, go, checker);
    Trinity::VisitNearbyGridObject(this, range, searcher);
    return go;
}

Player* WorldObject::FindNearestPlayer(float range, bool alive)
{
    Player* player = NULL;
    Trinity::AnyPlayerInObjectRangeCheck check(this, CalcVisibilityRange());
    Trinity::PlayerSearcher<Trinity::AnyPlayerInObjectRangeCheck> searcher(this, player, check);
    Trinity::VisitNearbyWorldObject(this, range, searcher);
    return player;
}

GameObject* WorldObject::FindNearestGameObjectOfType(GameobjectTypes type, float range) const
{ 
    GameObject* go = NULL;
    Trinity::NearestGameObjectTypeInObjectRangeCheck checker(*this, type, range);
    Trinity::GameObjectLastSearcher<Trinity::NearestGameObjectTypeInObjectRangeCheck> searcher(this, go, checker);
    Trinity::VisitNearbyGridObject(this, range, searcher);
    return go;
}

void WorldObject::GetGameObjectListWithEntryInGrid(std::list<GameObject*>& gameobjectList, uint32 entry, float maxSearchRange) const
{
    CellCoord pair(Trinity::ComputeCellCoord(this->GetPositionX(), this->GetPositionY()));
    Cell cell(pair);
    cell.SetNoCreate();

    Trinity::AllGameObjectsWithEntryInRange check(this, entry, maxSearchRange);
    Trinity::GameObjectListSearcher<Trinity::AllGameObjectsWithEntryInRange> searcher(this, gameobjectList, check);

    cell.Visit(pair, Trinity::makeGridVisitor(searcher), *(this->GetMap()), *this, maxSearchRange);
}

void WorldObject::GetCreatureListWithEntryInGrid(std::list<Creature*>& creatureList, uint32 entry, float maxSearchRange) const
{
    CellCoord pair(Trinity::ComputeCellCoord(this->GetPositionX(), this->GetPositionY()));
    Cell cell(pair);
    cell.SetNoCreate();

    Trinity::AllCreaturesOfEntryInRange check(this, entry, maxSearchRange);
    Trinity::CreatureListSearcher<Trinity::AllCreaturesOfEntryInRange> searcher(this, creatureList, check);

    cell.Visit(pair, Trinity::makeGridVisitor(searcher), *(this->GetMap()), *this, maxSearchRange);
}

void WorldObject::GetAreaTriggerListWithEntryInGrid(std::list<AreaTrigger*>& atList, uint32 entry, float maxSearchRange) const
{
    CellCoord pair(Trinity::ComputeCellCoord(this->GetPositionX(), this->GetPositionY()));
    Cell cell(pair);
    cell.SetNoCreate();

    Trinity::AllAreaTriggeresOfEntryInRange check(this, entry, maxSearchRange);
    Trinity::AreaTriggerListSearcher<Trinity::AllAreaTriggeresOfEntryInRange> searcher(this, atList, check);

    cell.Visit(pair, Trinity::makeGridVisitor(searcher), *(this->GetMap()), *this, maxSearchRange);
}

void WorldObject::GetPlayerListInGrid(std::list<Player*>& playerList, float maxSearchRange) const
{    
    Trinity::AnyPlayerInObjectRangeCheck checker(this, maxSearchRange);
    Trinity::PlayerListSearcher<Trinity::AnyPlayerInObjectRangeCheck> searcher(this, playerList, checker);
    Trinity::VisitNearbyWorldObject(this, maxSearchRange, searcher);
}

void WorldObject::GetGameObjectListWithEntryInGridAppend(std::list<GameObject*>& gameobjectList, uint32 entry, float maxSearchRange) const
{
    std::list<GameObject*> tempList;
    GetGameObjectListWithEntryInGrid(tempList, entry, maxSearchRange);
    gameobjectList.sort();
    tempList.sort();
    gameobjectList.merge(tempList);
}

void WorldObject::GetCreatureListWithEntryInGridAppend(std::list<Creature*>& creatureList, uint32 entry, float maxSearchRange) const
{
    std::list<Creature*> tempList;
    GetCreatureListWithEntryInGrid(tempList, entry, maxSearchRange);
    creatureList.sort();
    tempList.sort();
    creatureList.merge(tempList);
}

void WorldObject::GetAliveCreatureListWithEntryInGrid(std::list<Creature*>& creatureList, uint32 entry, float maxSearchRange) const
{
    CellCoord pair(Trinity::ComputeCellCoord(this->GetPositionX(), this->GetPositionY()));
    Cell cell(pair);
    cell.SetNoCreate();

    Trinity::AllAliveCreaturesOfEntryInRange check(this, entry, maxSearchRange);
    Trinity::CreatureListSearcher<Trinity::AllAliveCreaturesOfEntryInRange> searcher(this, creatureList, check);

    cell.Visit(pair, Trinity::makeGridVisitor(searcher), *(this->GetMap()), *this, maxSearchRange);
}

void WorldObject::GetCorpseCreatureInGrid(std::list<Creature*>& creatureList, float maxSearchRange) const
{
    CellCoord pair(Trinity::ComputeCellCoord(this->GetPositionX(), this->GetPositionY()));
    Cell cell(pair);
    cell.SetNoCreate();

    Trinity::SearchCorpseCreatureCheck check(this, maxSearchRange);
    Trinity::CreatureListSearcher<Trinity::SearchCorpseCreatureCheck> searcher(this, creatureList, check);

    cell.Visit(pair, Trinity::makeGridVisitor(searcher), *(this->GetMap()), *this, maxSearchRange);
}

//===================================================================================================

void WorldObject::GetNearPoint2D(float &x, float &y, float distance2d, float absAngle) const
{
    x = GetPositionX() + (GetObjectSize() + distance2d) * std::cos(absAngle);
    y = GetPositionY() + (GetObjectSize() + distance2d) * std::sin(absAngle);

    Trinity::NormalizeMapCoord(x);
    Trinity::NormalizeMapCoord(y);
}

void WorldObject::GetNearPoint2D(Position &pos, float distance2d, float angle) const
{
    angle += GetOrientation();
    float x = GetPositionX() + (GetObjectSize() + distance2d) * std::cos(angle);
    float y = GetPositionY() + (GetObjectSize() + distance2d) * std::sin(angle);

    Trinity::NormalizeMapCoord(x);
    Trinity::NormalizeMapCoord(y);

    pos.m_positionX = x;
    pos.m_positionY = y;
    pos.m_positionZ = GetPositionZ();

    float ground = pos.m_positionZ;

    UpdateAllowedPositionZ(x, y, ground);

    if (fabs(pos.m_positionZ - ground) < 6)
        pos.m_positionZ = ground;
}

void WorldObject::GetNearPoint(WorldObject const* searcher, float &x, float &y, float &z, float searcher_size, float distance2d, float absAngle) const
{
    GetNearPoint2D(x, y, distance2d+searcher_size, absAngle);
    z = GetPositionZH() + (searcher ? searcher->GetPositionH() : 0.0f);

    if (!searcher)
        UpdateAllowedPositionZ(x, y, z);
    else if (!searcher->ToCreature() || !searcher->GetMap()->Instanceable())
        searcher->UpdateAllowedPositionZ(x, y, z);
}

void WorldObject::MovePosition(Position &pos, float dist, float angle)
{
    angle += GetOrientation();
    float destx, desty, destz, ground, floor;
    destx = pos.m_positionX + dist * std::cos(angle);
    desty = pos.m_positionY + dist * std::sin(angle);

    // Prevent invalid coordinates here, position is unchanged
    if (!Trinity::IsValidMapCoord(destx, desty))
    {
        TC_LOG_FATAL("server", "WorldObject::MovePosition invalid coordinates X: %f and Y: %f were passed!", destx, desty);
        return;
    }

    ground = GetMap()->GetHeight(GetPhaseMask(), destx, desty, MAX_HEIGHT, true);
    floor = GetMap()->GetHeight(GetPhaseMask(), destx, desty, pos.m_positionZ, true);
    destz = fabs(ground - pos.m_positionZ) <= fabs(floor - pos.m_positionZ) ? ground : floor;

    float step = dist/10.0f;

    for (uint8 j = 0; j < 10; ++j)
    {
        // do not allow too big z changes
        if (fabs(pos.m_positionZ - destz) > 6)
        {
            destx -= step * std::cos(angle);
            desty -= step * std::sin(angle);
            ground = GetMap()->GetHeight(GetPhaseMask(), destx, desty, MAX_HEIGHT, true);
            floor = GetMap()->GetHeight(GetPhaseMask(), destx, desty, pos.m_positionZ, true);
            destz = fabs(ground - pos.m_positionZ) <= fabs(floor - pos.m_positionZ) ? ground : floor;
        }
        // we have correct destz now
        else
        {
            pos.Relocate(destx, desty, destz);
            break;
        }
    }

    Trinity::NormalizeMapCoord(pos.m_positionX);
    Trinity::NormalizeMapCoord(pos.m_positionY);
    UpdateGroundPositionZ(pos.m_positionX, pos.m_positionY, pos.m_positionZ);
    pos.SetOrientation(GetOrientation());
}

void WorldObject::MovePositionToFirstCollision(Position &pos, float dist, float angle)
{
    angle += GetOrientation();
    float destx, desty, destz, ground, floor;
    if (!IsInWater())
        pos.m_positionZ += 2.0f;
    destx = pos.m_positionX + dist * std::cos(angle);
    desty = pos.m_positionY + dist * std::sin(angle);

    // Prevent invalid coordinates here, position is unchanged
    if (!Trinity::IsValidMapCoord(destx, desty))
    {
        TC_LOG_FATAL("server", "WorldObject::MovePositionToFirstCollision invalid coordinates X: %f and Y: %f were passed!", destx, desty);
        return;
    }

    bool isFalling = m_movementInfo.HasMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
    ground = GetMap()->GetHeight(GetPhaseMask(), destx, desty, MAX_HEIGHT, true);
    floor = GetMap()->GetHeight(GetPhaseMask(), destx, desty, pos.m_positionZ, true);
    destz = fabs(ground - pos.m_positionZ) <= fabs(floor - pos.m_positionZ) ? ground : floor;

    if (IsInWater()) // In water not allow change Z to ground
    {
        if (pos.m_positionZ > destz)
            destz = pos.m_positionZ;
    }

    bool _checkZ = true;
    if (isFalling) // Allowed point in air if we falling
    {
        float z_now = m_movementInfo.lastTimeUpdate ? (pos.m_positionZ - Movement::computeFallElevation(Movement::MSToSec(getMSTime() - m_movementInfo.lastTimeUpdate), false) - 5.0f) : pos.m_positionZ;
        if ((z_now - ground) > 10.0f)
        {
            destz = z_now;
            _checkZ = false;
        }
    }

    bool col = VMAP::VMapFactory::createOrGetVMapManager()->getObjectHitPos(GetMapId(), pos.m_positionX, pos.m_positionY, pos.m_positionZ+0.5f, destx, desty, destz+0.5f, destx, desty, destz, -0.5f);

    // collision occurred
    if (col)
    {
        // move back a bit
        destx -= CONTACT_DISTANCE * std::cos(angle);
        desty -= CONTACT_DISTANCE * std::sin(angle);
        dist = sqrt((pos.m_positionX - destx)*(pos.m_positionX - destx) + (pos.m_positionY - desty)*(pos.m_positionY - desty));
    }
    else
        destz -= 0.5f;

    // check dynamic collision
    col = GetMap()->getObjectHitPos(GetPhaseMask(), pos.m_positionX, pos.m_positionY, pos.m_positionZ+0.5f, destx, desty, destz+0.5f, destx, desty, destz, -0.5f);

    // Collided with a gameobject
    if (col)
    {
        destx -= CONTACT_DISTANCE * std::cos(angle);
        desty -= CONTACT_DISTANCE * std::sin(angle);
        dist = sqrt((pos.m_positionX - destx)*(pos.m_positionX - destx) + (pos.m_positionY - desty)*(pos.m_positionY - desty));
    }
    else
        destz -= 0.5f;

    float step = dist/10.0f;

    for (uint8 j = 0; j < 10; ++j)
    {
        // do not allow too big z changes
        if (fabs(pos.m_positionZ - destz) > 6 && _checkZ)
        {
            destx -= step * std::cos(angle);
            desty -= step * std::sin(angle);
            ground = GetMap()->GetHeight(GetPhaseMask(), destx, desty, MAX_HEIGHT, true);
            floor = GetMap()->GetHeight(GetPhaseMask(), destx, desty, pos.m_positionZ, true);
            destz = fabs(ground - pos.m_positionZ) <= fabs(floor - pos.m_positionZ) ? ground : floor;
        }
        // we have correct destz now
        else
        {
            pos.Relocate(destx, desty, destz);
            break;
        }
    }

    Trinity::NormalizeMapCoord(pos.m_positionX);
    Trinity::NormalizeMapCoord(pos.m_positionY);
    UpdateAllowedPositionZ(pos.m_positionX, pos.m_positionY, pos.m_positionZ);
    pos.SetOrientation(GetOrientation());
}

void WorldObject::MovePositionToCollisionBetween(Position &pos, float distMin, float distMax, float angle)
{
    angle += GetOrientation();
    float destx, desty, destz, tempDestx, tempDesty, ground, floor;
    pos.m_positionZ += 2.0f;

    tempDestx = pos.m_positionX + distMin * std::cos(angle);
    tempDesty = pos.m_positionY + distMin * std::sin(angle);

    destx = pos.m_positionX + distMax * std::cos(angle);
    desty = pos.m_positionY + distMax * std::sin(angle);

    // Prevent invalid coordinates here, position is unchanged
    if (!Trinity::IsValidMapCoord(destx, desty))
    {
        TC_LOG_FATAL("server", "WorldObject::MovePositionToFirstCollision invalid coordinates X: %f and Y: %f were passed!", destx, desty);
        return;
    }

    ground = GetMap()->GetHeight(GetPhaseMask(), destx, desty, MAX_HEIGHT, true);
    floor = GetMap()->GetHeight(GetPhaseMask(), destx, desty, pos.m_positionZ, true);
    destz = fabs(ground - pos.m_positionZ) <= fabs(floor - pos.m_positionZ) ? ground : floor;

    bool col = VMAP::VMapFactory::createOrGetVMapManager()->getObjectHitPos(GetMapId(), tempDestx, tempDesty, pos.m_positionZ+0.5f, destx, desty, destz+0.5f, destx, desty, destz, -0.5f);

    // collision occurred
    if (col)
    {
        // move back a bit
        destx -= CONTACT_DISTANCE * std::cos(angle);
        desty -= CONTACT_DISTANCE * std::sin(angle);
        distMax = sqrt((pos.m_positionX - destx)*(pos.m_positionX - destx) + (pos.m_positionY - desty)*(pos.m_positionY - desty));
    }

    // check dynamic collision
    col = GetMap()->getObjectHitPos(GetPhaseMask(), tempDestx, tempDesty, pos.m_positionZ+0.5f, destx, desty, destz+0.5f, destx, desty, destz, -0.5f);

    // Collided with a gameobject
    if (col)
    {
        destx -= CONTACT_DISTANCE * std::cos(angle);
        desty -= CONTACT_DISTANCE * std::sin(angle);
        distMax = sqrt((pos.m_positionX - destx)*(pos.m_positionX - destx) + (pos.m_positionY - desty)*(pos.m_positionY - desty));
    }

    float step = distMax/10.0f;

    for (uint8 j = 0; j < 10; ++j)
    {
        // do not allow too big z changes
        if (fabs(pos.m_positionZ - destz) > 6)
        {
            destx -= step * std::cos(angle);
            desty -= step * std::sin(angle);
            ground = GetMap()->GetHeight(GetPhaseMask(), destx, desty, MAX_HEIGHT, true);
            floor = GetMap()->GetHeight(GetPhaseMask(), destx, desty, pos.m_positionZ, true);
            destz = fabs(ground - pos.m_positionZ) <= fabs(floor - pos.m_positionZ) ? ground : floor;
        }
        // we have correct destz now
        else
        {
            pos.Relocate(destx, desty, destz);
            break;
        }
    }

    Trinity::NormalizeMapCoord(pos.m_positionX);
    Trinity::NormalizeMapCoord(pos.m_positionY);
    UpdateAllowedPositionZ(pos.m_positionX, pos.m_positionY, pos.m_positionZ);
    pos.SetOrientation(GetOrientation());
}

void WorldObject::SetPhaseMask(uint32 newPhaseMask, bool update)
{
    m_phaseMask = newPhaseMask;

    if (update && IsInWorld())
        UpdateObjectVisibility();
}

//! 5.4.1
void WorldObject::PlayDistanceSound(uint32 sound_id, Player* target /*= NULL*/)
{
    ObjectGuid guid = GetGUID();
    ObjectGuid obj_guid = target ? target->GetGUID() : 0;
    
    WorldPacket data(SMSG_PLAY_OBJECT_SOUND, 10);
    data.WriteGuidMask<5, 3>(obj_guid);
    data.WriteGuidMask<6, 5>(guid);
    data.WriteGuidMask<6>(obj_guid);
    data.WriteGuidMask<0, 2>(guid);
    data.WriteGuidMask<1>(obj_guid);
    data.WriteGuidMask<7>(guid);
    data.WriteGuidMask<2>(obj_guid);
    data.WriteGuidMask<4, 3>(guid);
    data.WriteGuidMask<4>(obj_guid);
    data.WriteGuidMask<1>(guid);
    data.WriteGuidMask<7, 0>(obj_guid);

    data.WriteGuidBytes<0>(guid);
    data.WriteGuidBytes<2, 7>(obj_guid);
    data.WriteGuidBytes<5>(guid);
    data.WriteGuidBytes<5>(obj_guid);
    data.WriteGuidBytes<3>(guid);
    data.WriteGuidBytes<0, 4>(obj_guid);
    data.WriteGuidBytes<4, 1, 6, 7>(guid);
    data.WriteGuidBytes<3, 6>(obj_guid);
    data << uint32(sound_id);
    data.WriteGuidBytes<1>(obj_guid);
    data.WriteGuidBytes<2>(guid);

    if (target)
        target->SendDirectMessage(&data);
    else
        SendMessageToSet(&data, true);
}

//! 5.4.1
void WorldObject::PlayDirectSound(uint32 sound_id, Player* target /*= NULL*/)
{
    ObjectGuid source = GetGUID();
    WorldPacket data(SMSG_PLAY_SOUND, 12);
    data.WriteGuidMask<0, 2, 4, 7, 6, 5, 1, 3>(source);
    data.WriteGuidBytes<3, 4, 2, 6, 1, 5, 0>(source);
    data << uint32(sound_id);
    data.WriteGuidBytes<7>(source);

    if (target)
        target->SendDirectMessage(&data);
    else
        SendMessageToSet(&data, true);
}

void WorldObject::DestroyForNearbyPlayers()
{
    if (!IsInWorld())
        return;

    std::list<Player*> targets;
    float radius = CalcVisibilityRange();
    Trinity::AnyPlayerInObjectRangeCheck check(this, radius, false);
    Trinity::PlayerListSearcher<Trinity::AnyPlayerInObjectRangeCheck> searcher(this, targets, check);
    Trinity::VisitNearbyWorldObject(this, radius, searcher);
    for (std::list<Player*>::const_iterator iter = targets.begin(); iter != targets.end(); ++iter)
    {
        Player* player = (*iter);

        if (player == this)
            continue;

        if (!player->HaveAtClient(this))
            continue;

        if (isType(TYPEMASK_UNIT) && ((Unit*)this)->GetCharmerGUID() == player->GetGUID()) // TODO: this is for puppet
            continue;

        DestroyForPlayer(player);
        player->m_clientGUIDs.erase(GetGUID());
    }
}

void WorldObject::DestroyVignetteForNearbyPlayers()
{
    if (!IsInWorld() || !GetVignetteId())
        return;

    std::list<Player*> targets;
    Trinity::AnyPlayerInObjectRangeCheck check(this, CalcVisibilityRange(), false);
    Trinity::PlayerListSearcher<Trinity::AnyPlayerInObjectRangeCheck> searcher(this, targets, check);
    Trinity::VisitNearbyWorldObject(this, CalcVisibilityRange(), searcher);
    for (std::list<Player*>::const_iterator iter = targets.begin(); iter != targets.end(); ++iter)
    {
        Player* player = (*iter);

        if (player == this)
            continue;

        if (!player->HaveAtClient(this))
            continue;

        if (isType(TYPEMASK_UNIT) && ((Unit*)this)->GetCharmerGUID() == player->GetGUID()) // TODO: this is for puppet
            continue;

        player->RemoveVignette(this, true);
    }
}

void WorldObject::UpdateObjectVisibility(bool /*forced*/, float customVisRange)
{
    //updates object's visibility for nearby players
    Trinity::VisibleChangesNotifier notifier(*this);
    Trinity::VisitNearbyWorldObject(this, CalcVisibilityRange(), notifier);
}

struct WorldObjectChangeAccumulator
{
    UpdateDataMapType& i_updateDatas;
    WorldObject& i_object;
    std::set<uint64> plr_list;
        WorldObjectChangeAccumulator(WorldObject &obj, UpdateDataMapType &d)
        : i_updateDatas(d), i_object(obj)
    { }

    void Visit(PlayerMapType &m)
    {
        for (auto &source : m)
        {
            BuildPacket(source);

            if (!source->GetSharedVisionList().empty())
                for (auto &player : source->GetSharedVisionList())
                    BuildPacket(player);
        }
    }

    void Visit(CreatureMapType &m)
    {
        for (auto &source : m)
        {
            if (!source->GetSharedVisionList().empty())
                for (auto &player : source->GetSharedVisionList())
                    BuildPacket(player);
        }
    }

    void Visit(DynamicObjectMapType &m)
    {
        for (auto &source : m)
        {
            auto const guid = source->GetCasterGUID();
            if (!IS_PLAYER_GUID(guid))
                continue;

            // Caster may be NULL if DynObj is in removelist
            auto const caster = ObjectAccessor::FindPlayer(guid);
            if (caster && caster->GetUInt64Value(PLAYER_FARSIGHT) == source->GetGUID())
                BuildPacket(caster);
        }
    }

    void BuildPacket(Player* player)
    {
        // Only send update once to a player
        if (plr_list.find(player->GetGUID()) == plr_list.end() && player->HaveAtClient(&i_object))
        {
            i_object.BuildFieldsUpdate(player, i_updateDatas);
            plr_list.insert(player->GetGUID());
        }
    }

    template <typename NotInterested>
    void Visit(NotInterested &) { }
};

void WorldObject::BuildUpdate(UpdateDataMapType& data_map)
{
    CellCoord p = Trinity::ComputeCellCoord(GetPositionX(), GetPositionY());
    Cell cell(p);
    cell.SetNoCreate();
    WorldObjectChangeAccumulator notifier(*this, data_map);

    //we must build packets for all visible players
    cell.Visit(p, Trinity::makeWorldVisitor(notifier), *GetMap(), *this, CalcVisibilityRange());

    ClearUpdateMask(false);
}

uint64 WorldObject::GetTransGUID() const
{
    if (GetTransport())
        return GetTransport()->GetGUID();
    return 0;
}

C_PTR Object::get_ptr()
{
    if (ptr.numerator && ptr.numerator->ready)
        return ptr.shared_from_this();

    ptr.InitParent(this);
    ASSERT(ptr.numerator);  // It's very bad. If it hit nothing work.
    return ptr.shared_from_this();
}

template<class NOTIFIER>
void WorldObject::VisitNearbyObject(const float &radius, NOTIFIER &notifier) const 
{
    if (IsInWorld())
        GetMap()->VisitAll(GetPositionX(), GetPositionY(), radius, notifier);
}

template<class NOTIFIER> 
void WorldObject::VisitNearbyGridObject(const float &radius, NOTIFIER &notifier) const 
{
    if (IsInWorld())
        GetMap()->VisitGrid(GetPositionX(), GetPositionY(), radius, notifier);
}

template<class NOTIFIER> 
void WorldObject::VisitNearbyWorldObject(const float &radius, NOTIFIER &notifier) const 
{
    if (IsInWorld())
        GetMap()->VisitWorld(GetPositionX(), GetPositionY(), radius, notifier);
}

void WorldObject::GetNearPosition(Position &pos, float dist, float angle, bool withinLOS)
{
    GetPosition(&pos);

    if (withinLOS)
    {
        Position savePos = pos;
        MovePosition(pos, dist, angle);

        if (!IsWithinLOS(pos.m_positionX, pos.m_positionY, pos.m_positionZ))
        {
            float stepOfangle = static_cast<float>(2 * M_PI) / 8.0f;
            uint8 reSteps = 0;
            bool goToCheck = false;

            for (uint8 i = 0; i < 8; i++)
            {
                if (!goToCheck)
                {
                    reSteps = i;
                    if (stepOfangle * (i + 1) > angle)
                        goToCheck = true;
                }

                if (goToCheck)
                {
                    pos = savePos;
                    MovePosition(pos, dist, stepOfangle * (i + 1));

                    if (IsWithinLOS(pos.m_positionX, pos.m_positionY, pos.m_positionZ))
                    {
                        pos.SetOrientation(pos.GetAngle(GetPositionX(), GetPositionY()));
                        return;
                    }
                }
            }

            for (uint8 q = 0; q <= reSteps; q++)
            {
                pos = savePos;
                MovePosition(pos, dist, stepOfangle * q);

                if (IsWithinLOS(pos.m_positionX, pos.m_positionY, pos.m_positionZ))
                {
                    pos.SetOrientation(pos.GetAngle(GetPositionX(), GetPositionY()));
                    return;
                }
            }

            pos = savePos;
            MovePosition(pos, 0.1f, angle);
        }
        pos.SetOrientation(pos.GetAngle(GetPositionX(), GetPositionY()));
    }
    else
        MovePosition(pos, dist, angle);
}
