#include <Services/CombatService.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/ProjectileLaunchedEvent.h>
#include <Events/HitEvent.h>

#include <Messages/ProjectileLaunchRequest.h>
#include <Messages/NotifyProjectileLaunch.h>

#include <Systems/ModSystem.h>
#include <World.h>

#include <Projectiles/Projectile.h>
#include <Forms/TESObjectWEAP.h>
#include <Forms/TESAmmo.h>
#include <Games/ActorExtension.h>
#include <Games/References.h>

CombatService::CombatService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CombatService::OnUpdate>(this);
    m_hitConnection = aDispatcher.sink<HitEvent>().connect<&CombatService::OnHitEvent>(this);
    m_localComponentRemoved = m_world.on_destroy<LocalComponent>().connect<&CombatService::OnLocalComponentRemoved>(this);
    m_projectileLaunchedConnection = aDispatcher.sink<ProjectileLaunchedEvent>().connect<&CombatService::OnProjectileLaunchedEvent>(this);
    m_projectileLaunchConnection = aDispatcher.sink<NotifyProjectileLaunch>().connect<&CombatService::OnNotifyProjectileLaunch>(this);
}

void CombatService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    RunTargetUpdates(static_cast<float>(acEvent.Delta));
    RunPvpCombatUpdates();
}

void CombatService::OnLocalComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    m_world.remove<CombatComponent>(aEntity);
}

void CombatService::OnProjectileLaunchedEvent(const ProjectileLaunchedEvent& acEvent) const noexcept
{
    ModSystem& modSystem = m_world.Get().GetModSystem();

    uint32_t shooterFormId = acEvent.ShooterID;
    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto shooterEntityIt = std::find_if(std::begin(view), std::end(view), [shooterFormId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == shooterFormId; });

    if (shooterEntityIt == std::end(view))
        return;

    LocalComponent& localComponent = view.get<LocalComponent>(*shooterEntityIt);

    ProjectileLaunchRequest request{};

    request.OriginX = acEvent.Origin.x;
    request.OriginY = acEvent.Origin.y;
    request.OriginZ = acEvent.Origin.z;

    modSystem.GetServerModId(acEvent.ProjectileBaseID, request.ProjectileBaseID);
    modSystem.GetServerModId(acEvent.WeaponID, request.WeaponID);
    modSystem.GetServerModId(acEvent.AmmoID, request.AmmoID);

    request.ShooterID = localComponent.Id;

    request.ZAngle = acEvent.ZAngle;
    request.XAngle = acEvent.XAngle;
    request.YAngle = acEvent.YAngle;

    modSystem.GetServerModId(acEvent.ParentCellID, request.ParentCellID);
    modSystem.GetServerModId(acEvent.SpellID, request.SpellID);

    request.CastingSource = acEvent.CastingSource;

    request.Area = acEvent.Area;
    request.Power = acEvent.Power;
    request.Scale = acEvent.Scale;

    request.AlwaysHit = acEvent.AlwaysHit;
    request.NoDamageOutsideCombat = acEvent.NoDamageOutsideCombat;
    request.AutoAim = acEvent.AutoAim;
    request.DeferInitialization = acEvent.DeferInitialization;
    request.ForceConeOfFire = acEvent.ForceConeOfFire;

    request.UnkBool1 = acEvent.UnkBool1;
    request.UnkBool2 = acEvent.UnkBool2;

    m_transport.Send(request);
}

void CombatService::OnNotifyProjectileLaunch(const NotifyProjectileLaunch& acMessage) const noexcept
{
    ModSystem& modSystem = World::Get().GetModSystem();

    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();
    const auto remoteIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.ShooterID](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteIt == std::end(remoteView))
    {
        spdlog::warn("Shooter with remote id {:X} not found.", acMessage.ShooterID);
        return;
    }

    FormIdComponent formIdComponent = remoteView.get<FormIdComponent>(*remoteIt);

    Projectile::LaunchData launchData{};

    // Projectile::Launch relies on pParentCell being valid.
    // TODO: it's possible that more of these values must have a value, should probably check for that in the game code.
    const uint32_t cParentCellId = modSystem.GetGameId(acMessage.ParentCellID);
    launchData.pParentCell = Cast<TESObjectCELL>(TESForm::GetById(cParentCellId));

    if (!launchData.pParentCell)
    {
        spdlog::warn("Cannot launch projectile, invalid parent cell: {:X}", cParentCellId);
        return;
    }

    launchData.pShooter = Cast<TESObjectREFR>(TESForm::GetById(formIdComponent.Id));

    launchData.Origin.x = acMessage.OriginX;
    launchData.Origin.y = acMessage.OriginY;
    launchData.Origin.z = acMessage.OriginZ;

    const uint32_t cProjectileBaseId = modSystem.GetGameId(acMessage.ProjectileBaseID);
    launchData.pProjectileBase = TESForm::GetById(cProjectileBaseId);

    const uint32_t cFromWeaponId = modSystem.GetGameId(acMessage.WeaponID);
    launchData.pFromWeapon = Cast<TESObjectWEAP>(TESForm::GetById(cFromWeaponId));

    const uint32_t cFromAmmoId = modSystem.GetGameId(acMessage.AmmoID);
    launchData.pFromAmmo = Cast<TESAmmo>(TESForm::GetById(cFromAmmoId));

    launchData.fZAngle = acMessage.ZAngle;
    launchData.fXAngle = acMessage.XAngle;
    launchData.fYAngle = acMessage.YAngle;

    const uint32_t cSpellId = modSystem.GetGameId(acMessage.SpellID);
    launchData.pSpell = Cast<MagicItem>(TESForm::GetById(cSpellId));

    launchData.eCastingSource = (MagicSystem::CastingSource)acMessage.CastingSource;

    launchData.iArea = acMessage.Area;
    launchData.fPower = acMessage.Power;
    launchData.fScale = acMessage.Scale;

    launchData.bAlwaysHit = acMessage.AlwaysHit;
    launchData.bNoDamageOutsideCombat = acMessage.NoDamageOutsideCombat;
    launchData.bAutoAim = acMessage.AutoAim;

    launchData.bForceConeOfFire = acMessage.ForceConeOfFire;

    // always use origin, or it'll recalculate it and it desyncs
    launchData.bUseOrigin = true;

    launchData.bUnkBool1 = acMessage.UnkBool1;
    launchData.bUnkBool2 = acMessage.UnkBool2;

    BSPointerHandle<Projectile> result;

    Projectile::Launch(&result, launchData);
}

void CombatService::OnHitEvent(const HitEvent& acEvent) noexcept
{
    // Health bars: the game only draws one for an actor it treats as an
    // opponent. Remote players are in the player faction, so a hit between
    // players is not combat as far as the HUD is concerned. Entering real
    // combat fixes that for the vanilla HUD and for any health bar mod,
    // without touching factions, Essential or the bleedout behaviour.
    if (m_transport.IsConnected() && World::Get().GetServerSettings().PvpEnabled)
    {
        auto* pHitter = Cast<Actor>(TESForm::GetById(acEvent.HitterId));
        auto* pHittee = Cast<Actor>(TESForm::GetById(acEvent.HitteeId));

        if (pHitter && pHittee)
        {
            const auto* pExHitter = pHitter->GetExtension();
            const auto* pExHittee = pHittee->GetExtension();

            // Only player-versus-player, and only pairs involving us: the
            // local player is the one whose HUD and combat state we can drive.
            if (pExHitter->IsLocalPlayer() && pExHittee->IsRemotePlayer())
                EnterPvpCombat(pHitter, pHittee);
            else if (pExHitter->IsRemotePlayer() && pExHittee->IsLocalPlayer())
                EnterPvpCombat(pHittee, pHitter);
        }
    }

#if 0
    if (!m_transport.IsConnected())
        return;

    // The targeting system does not apply to players, and only local actors should be considered.
    auto* pHittee = Cast<Actor>(TESForm::GetById(acEvent.HitteeId));
    if (!pHittee || pHittee->GetExtension()->IsPlayer() || pHittee->GetExtension()->IsRemote())
        return;

    // NPCs should not affect targeting.
    auto* pHitter = Cast<Actor>(TESForm::GetById(acEvent.HitterId));
    if (!pHitter || !pHitter->GetExtension()->IsPlayer())
        return;

    // If the target is not in combat, don't start combat, let the game take care of that first.
    if (!pHittee->IsInCombat())
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent>(entt::exclude<ObjectComponent>);

    const auto hitteeIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.HitteeId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (hitteeIt == std::end(view))
    {
        spdlog::warn(__FUNCTION__ ": hittee form id component not found, form id: {:X}", acEvent.HitterId);
        return;
    }

    if (m_world.any_of<CombatComponent>(*hitteeIt))
        return;

    m_world.emplace_or_replace<CombatComponent>(*hitteeIt, acEvent.HitterId);

    pHittee->SetCombatTargetEx(pHitter);
#endif
}

void CombatService::EnterPvpCombat(Actor* apLocal, Actor* apRemote) noexcept
{
    auto& engagement = m_pvpEngagements[apRemote->formID];
    engagement.lastDamage = std::chrono::steady_clock::now();
    engagement.remoteHandle = apRemote->GetHandle().handle.iBits;

    // StartCombat is still useful for all of the normal combat events, but it
    // can reject remote players because they share the player faction. Force
    // the current targets afterwards: the vanilla EnemyHealth HUD reads the
    // local player's currentCombatTarget directly.
    apLocal->StartCombatEx(apRemote);
    apRemote->StartCombatEx(apLocal);
    apLocal->SetCombatTargetEx(apRemote);
    apRemote->SetCombatTargetEx(apLocal);

    // Logged at info: without it there is no way to tell from a test session
    // whether this ran at all, and whether the game kept what we set. The
    // vanilla EnemyHealth HUD reads the local player's combat target, so that
    // is the thing that has to stick.
    //
    // The faction list is included because SetFactions runs after
    // SetPlayerRespawnMode during spawn and starts with RemoveFromAllFactions,
    // so it is not obvious whether a remote player actually ends up in the
    // player faction (0xDB1) at all. That decides whether the remaining blocker
    // is hostility or something else entirely.
    const auto* pKeptTarget = apLocal->GetCombatTarget();
    const auto remoteFactions = apRemote->GetFactions();

    bool inPlayerFaction = false;
    for (const auto& faction : remoteFactions.NpcFactions)
    {
        if (faction.Id.BaseId == 0xDB1)
            inPlayerFaction = true;
    }

    spdlog::info("[pvp-hud] local {:X} vs remote {:X}: targetKept={} localInCombat={} remoteInCombat={} essential={} ignoreFriendly={} playerFaction={} npcFactions={} extraFactions={}",
                 apLocal->formID, apRemote->formID, pKeptTarget == apRemote, apLocal->IsInCombat(), apRemote->IsInCombat(), apRemote->IsEssential(), apRemote->GetIgnoreFriendlyHit(),
                 inPlayerFaction, remoteFactions.NpcFactions.size(), remoteFactions.ExtraFactions.size());
}

void CombatService::RunPvpCombatUpdates() noexcept
{
    if (m_pvpEngagements.empty())
        return;

    // No damage traded for this long ends it.
    constexpr auto cPvpTimeout = 60s;

    auto* pLocalPlayer = PlayerCharacter::Get();
    if (!pLocalPlayer)
    {
        m_pvpEngagements.clear();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool localSheathed = !pLocalPlayer->actorState.IsWeaponDrawn();

    // Map's iterators expose the value as const, so decide first and apply the
    // changes afterwards through operator[] / erase.
    Vector<uint32_t> ended;
    Vector<uint32_t> sawWeapons;
    Actor* pHudTarget = nullptr;
    auto latestDamage = std::chrono::steady_clock::time_point::min();

    for (const auto& [remoteFormId, engagement] : m_pvpEngagements)
    {
        auto* pRemote = Cast<Actor>(TESForm::GetById(remoteFormId));

        // The remote player is gone, gave up its remote status, or died.
        bool isOver = !pRemote || !pRemote->GetExtension()->IsRemotePlayer();

        if (!isOver)
            isOver = pRemote->IsDead() || pLocalPlayer->IsDead();

        if (!isOver)
            isOver = now - engagement.lastDamage >= cPvpTimeout;

        // Both sides putting their weapons away is an explicit "we are done",
        // but only once we have actually seen them drawn: a fist or spell fight
        // never draws a weapon and would otherwise end on the first frame.
        const bool remoteDrawn = pRemote && pRemote->actorState.IsWeaponDrawn();
        const bool weaponsOut = !localSheathed || remoteDrawn;

        if (!isOver && engagement.sawWeaponsDrawn)
            isOver = localSheathed && !remoteDrawn;

        if (!isOver)
        {
            // Skyrim's target selector can clear a same-faction target on its
            // next update. Reasserting it here is normally just two integer
            // comparisons, and keeps the remote player's controller aimed at
            // the local player for the lifetime of this engagement.
            pRemote->SetCombatTargetEx(pLocalPlayer);

            // The vanilla HUD has one enemy bar. If several players are
            // engaged, display the one involved in the most recent hit.
            if (!pHudTarget || engagement.lastDamage > latestDamage)
            {
                pHudTarget = pRemote;
                latestDamage = engagement.lastDamage;
            }

            // Only record it for a fight that is still running: operator[] would
            // otherwise resurrect an entry that is about to be erased.
            if (weaponsOut && !engagement.sawWeaponsDrawn)
                sawWeapons.push_back(remoteFormId);
            continue;
        }

        if (pLocalPlayer->combatHandle == engagement.remoteHandle)
        {
            pLocalPlayer->SetCombatTargetEx(nullptr);
            pLocalPlayer->StopCombat();
        }

        if (pRemote)
        {
            if (pRemote->GetCombatTarget() == pLocalPlayer)
            {
                pRemote->SetCombatTargetEx(nullptr);
                pRemote->StopCombat();
            }
        }

        ended.push_back(remoteFormId);
    }

    for (const uint32_t cRemoteFormId : sawWeapons)
        m_pvpEngagements[cRemoteFormId].sawWeaponsDrawn = true;

    for (const uint32_t cRemoteFormId : ended)
        m_pvpEngagements.erase(cRemoteFormId);

    if (pHudTarget)
        pLocalPlayer->SetCombatTargetEx(pHudTarget);
}

void CombatService::RunTargetUpdates(const float acDelta) const noexcept
{
#if 0
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 200ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    const auto view = m_world.view<FormIdComponent, CombatComponent>();

    Vector<entt::entity> toRemove{};

    for (const auto entity : view)
    {
        auto& combatComponent = view.get<CombatComponent>(entity);
        combatComponent.Timer = combatComponent.Timer - acDelta;

        if (combatComponent.Timer <= 0.f)
        {
            toRemove.push_back(entity);
            continue;
        }

        auto* pTarget = Cast<Actor>(TESForm::GetById(combatComponent.TargetFormId));
        if (!pTarget)
        {
            spdlog::warn(__FUNCTION__ ": combat target not found, form id {:X}", combatComponent.TargetFormId);
            toRemove.push_back(entity);
            continue;
        }

        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        auto* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
        {
            spdlog::error(__FUNCTION__ ": actor not found, form id {:X}", formIdComponent.Id);
            toRemove.push_back(entity);
            continue;
        }
    }

    for (const auto entity : toRemove)
        m_world.remove<CombatComponent>(entity);
#endif
}
