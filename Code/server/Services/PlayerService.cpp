#include "Events/CharacterInteriorCellChangeEvent.h"
#include "Events/CharacterExteriorCellChangeEvent.h"
#include "Events/PlayerLeaveCellEvent.h"

#include <Services/PlayerService.h>
#include <Services/CharacterService.h>
#include <GameServer.h>

#include <Messages/ShiftGridCellRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/PlayerRespawnRequest.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/NotifyPlayerRespawn.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/PlayerLevelRequest.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerCellChanged.h>

#include <Setting.h>
namespace
{
Console::Setting fGoldLossFactor{"Gameplay:fGoldLossFactor", "Factor of the amount of gold lost on death", 0.0f};

void SendVisibilityChanges(World& aWorld, Player* apPlayer, const CellIdComponent& acOldCell, const CellIdComponent& acNewCell) noexcept
{
    auto characterView = aWorld.view<CellIdComponent, CharacterComponent, OwnerComponent>();

    for (auto character : characterView)
    {
        const auto& ownerComponent = characterView.get<OwnerComponent>(character);
        if (ownerComponent.GetOwner() == apPlayer)
            continue;

        const auto& characterCell = characterView.get<CellIdComponent>(character);
        const auto& characterComponent = characterView.get<CharacterComponent>(character);
        const bool isDragon = characterComponent.IsDragon();

        const bool wasVisible = static_cast<bool>(acOldCell) && characterCell.IsInRange(acOldCell, isDragon);
        const bool isVisible = static_cast<bool>(acNewCell) && characterCell.IsInRange(acNewCell, isDragon);

        if (wasVisible == isVisible)
            continue;

        if (isVisible)
        {
            CharacterSpawnRequest spawnMessage;
            CharacterService::Serialize(aWorld, character, &spawnMessage);
            apPlayer->Send(spawnMessage);
            spdlog::info("[interest] player {:X} <- SPAWN character {:X} (grid {},{} -> {},{})", apPlayer->GetId(), World::ToInteger(character), acOldCell.CenterCoords.X, acOldCell.CenterCoords.Y,
                         acNewCell.CenterCoords.X, acNewCell.CenterCoords.Y);
        }
        else
        {
            NotifyRemoveCharacter removeMessage;
            removeMessage.ServerId = World::ToInteger(character);
            apPlayer->Send(removeMessage);
            spdlog::info("[interest] player {:X} <- REMOVE character {:X} (grid {},{} -> {},{})", apPlayer->GetId(), World::ToInteger(character), acOldCell.CenterCoords.X, acOldCell.CenterCoords.Y,
                         acNewCell.CenterCoords.X, acNewCell.CenterCoords.Y);
        }
    }
}
} // namespace

PlayerService::PlayerService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_gridCellShiftConnection(aDispatcher.sink<PacketEvent<ShiftGridCellRequest>>().connect<&PlayerService::HandleGridCellShift>(this))
    , m_exteriorCellEnterConnection(aDispatcher.sink<PacketEvent<EnterExteriorCellRequest>>().connect<&PlayerService::HandleExteriorCellEnter>(this))
    , m_interiorCellEnterConnection(aDispatcher.sink<PacketEvent<EnterInteriorCellRequest>>().connect<&PlayerService::HandleInteriorCellEnter>(this))
    , m_playerRespawnConnection(aDispatcher.sink<PacketEvent<PlayerRespawnRequest>>().connect<&PlayerService::OnPlayerRespawnRequest>(this))
    , m_playerLevelConnection(aDispatcher.sink<PacketEvent<PlayerLevelRequest>>().connect<&PlayerService::OnPlayerLevelRequest>(this))
{
}

void SendPlayerCellChanged(const Player* apPlayer) noexcept
{
    auto& cellComponent = apPlayer->GetCellComponent();

    NotifyPlayerCellChanged notify{};
    notify.PlayerId = apPlayer->GetId();
    notify.WorldSpaceId = cellComponent.WorldSpaceId;
    notify.CellId = cellComponent.Cell;

    GameServer::Get()->SendToPlayers(notify, apPlayer);
}

void PlayerService::HandleGridCellShift(const PacketEvent<ShiftGridCellRequest>& acMessage) const noexcept
{
    auto* pPlayer = acMessage.pPlayer;

    auto& message = acMessage.Packet;

    const CellIdComponent oldCell = pPlayer->GetCellComponent();
    const CellIdComponent newCell{message.PlayerCell, message.WorldSpaceId, message.CenterCoords};
    pPlayer->SetCellComponent(newCell);

    if (const auto playerCharacter = pPlayer->GetCharacter())
    {
        if (auto* pCharacterCell = m_world.try_get<CellIdComponent>(*playerCharacter))
        {
            pCharacterCell->Cell = message.PlayerCell;
            pCharacterCell->WorldSpaceId = message.WorldSpaceId;

            // Movement snapshots hold the actor's exact grid coordinates.
            // Only use the loaded-grid center as a fallback during the first
            // shift or a world-space transition.
            if (!oldCell || oldCell.WorldSpaceId != message.WorldSpaceId)
                pCharacterCell->CenterCoords = message.CenterCoords;
        }
    }

    if (oldCell)
        m_world.GetDispatcher().trigger(PlayerLeaveCellEvent(oldCell.Cell));

    // Compute both sides of the interest transition. The previous code only
    // spawned the newly loaded fringe and never removed entities that left it.
    SendVisibilityChanges(m_world, pPlayer, oldCell, newCell);
}

void PlayerService::HandleExteriorCellEnter(const PacketEvent<EnterExteriorCellRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    auto* pPlayer = acMessage.pPlayer;

    if (pPlayer->GetCharacter())
    {
        auto entity = *pPlayer->GetCharacter();
        const CellIdComponent oldCell = pPlayer->GetCellComponent();

        // CurrentCoords is the actor's current cell, not necessarily the
        // center of the 5x5 grid loaded by this client. Preserve the center
        // received through ShiftGridCellRequest whenever it is available.
        GridCellCoords loadedGridCenter = message.CurrentCoords;
        if (oldCell && oldCell.WorldSpaceId == message.WorldSpaceId)
            loadedGridCenter = oldCell.CenterCoords;

        const CellIdComponent newCell{message.CellId, message.WorldSpaceId, loadedGridCenter};
        pPlayer->SetCellComponent(newCell);

        if (auto* pCharacterCell = m_world.try_get<CellIdComponent>(entity))
        {
            pCharacterCell->Cell = message.CellId;
            pCharacterCell->WorldSpaceId = message.WorldSpaceId;
            pCharacterCell->CenterCoords = message.CurrentCoords;
        }

        SendVisibilityChanges(m_world, pPlayer, oldCell, newCell);

        if (oldCell)
            m_world.GetDispatcher().trigger(CharacterExteriorCellChangeEvent{pPlayer, entity, message.WorldSpaceId, message.CurrentCoords});

        SendPlayerCellChanged(pPlayer);
    }
}

void PlayerService::HandleInteriorCellEnter(const PacketEvent<EnterInteriorCellRequest>& acMessage) const noexcept
{
    auto* pPlayer = acMessage.pPlayer;

    auto& message = acMessage.Packet;

    const CellIdComponent oldCell = pPlayer->GetCellComponent();

    const CellIdComponent newCell{message.CellId, {}, {}};
    pPlayer->SetCellComponent(newCell);

    if (oldCell)
        m_world.GetDispatcher().trigger(PlayerLeaveCellEvent(oldCell.Cell));

    if (pPlayer->GetCharacter())
    {
        auto entity = *pPlayer->GetCharacter();

        if (auto* pCellIdComponent = m_world.try_get<CellIdComponent>(entity))
        {
            *pCellIdComponent = newCell;
            m_world.GetDispatcher().trigger(CharacterInteriorCellChangeEvent{pPlayer, entity, message.CellId});
        }
    }

    SendVisibilityChanges(m_world, pPlayer, oldCell, newCell);

    SendPlayerCellChanged(pPlayer);
}

void PlayerService::OnPlayerRespawnRequest(const PacketEvent<PlayerRespawnRequest>& acMessage) const noexcept
{
    float goldLossFactor = fGoldLossFactor.as_float();

    auto character = acMessage.pPlayer->GetCharacter();
    if (!character)
        return;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(*character));

    if (it != view.end())
    {
        if (goldLossFactor != 0.0)
        {
            auto& inventoryComponent = view.get<InventoryComponent>(*it);

            GameId goldId(0, 0xF);
            int32_t goldCount = inventoryComponent.Content.GetEntryCountById(goldId);
            int32_t goldToRemove = static_cast<int32_t>(goldCount * goldLossFactor);

            Inventory::Entry entry{};
            entry.BaseId = goldId;
            entry.Count = -goldToRemove;

            inventoryComponent.Content.AddOrRemoveEntry(entry);

            NotifyInventoryChanges notifyInventoryChanges{};
            notifyInventoryChanges.ServerId = World::ToInteger(*character);
            notifyInventoryChanges.Item = entry;
            notifyInventoryChanges.Drop = false;

            // Exclude respawned player from inventory changes notification...
            if (!GameServer::Get()->SendToPlayersInRange(notifyInventoryChanges, *character, acMessage.GetSender()))
                spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);

            // ...and instead, send NotifyPlayerRespawn so that the client can print a message.
            NotifyPlayerRespawn notifyPlayerRespawn{};
            notifyPlayerRespawn.GoldLost = goldToRemove;

            acMessage.pPlayer->Send(notifyPlayerRespawn);
        }

        // Let all other players in cell respawn this player, since the body state seems to be bugged otherwise
        NotifyRespawn notifyRespawn{};
        notifyRespawn.ActorId = World::ToInteger(*character);

        if (!GameServer::Get()->SendToPlayersInRange(notifyRespawn, *character, acMessage.GetSender()))
            spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
    }
}

void PlayerService::OnPlayerLevelRequest(const PacketEvent<PlayerLevelRequest>& acMessage) const noexcept
{
    acMessage.pPlayer->SetLevel(acMessage.Packet.NewLevel);

    NotifyPlayerLevel notify{};
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.NewLevel = acMessage.Packet.NewLevel;

    GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);
}
