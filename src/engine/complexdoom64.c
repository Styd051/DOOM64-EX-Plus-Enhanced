// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// 2026 Styd051
// 
// Complex DOOM 64 Game Mode
//
//-----------------------------------------------------------------------------

#include <stddef.h>
#include "complexdoom64.h"
#include "m_random.h"
#include "con_cvar.h"

#define ARRAY_COUNT(x) (sizeof(x) / sizeof((x)[0]))

CVAR(m_complexdoom64, 0);
CVAR(m_complexdoom64_difficulty, 0);
CVAR(m_complexdoom64_nightmare, 1);

typedef struct
{
    mobjtype_t type;
    int weight;    
} complex_weighted_t;

enum
{
    C64DIFF_NORMAL = 0,
    C64DIFF_HARDCORE,
    C64DIFF_CHAOS
};

typedef enum
{
    C64GRP_ZOMBIEMAN,
    C64GRP_ZOMBIEMANSHOTGUN,
    C64GRP_IMP,
    C64GRP_IMPNIGHTMARE,
    C64GRP_DEMON,
    C64GRP_SPECTRE,
    C64GRP_CACODEMON,
    C64GRP_PAINELEMENTAL,
    C64GRP_BARON,
    C64GRP_HELLKNIGHT,
    C64GRP_CYBERDEMON,
    C64GRP_MOTHERDEMON,
    C64GRP_AMMO_CLIP,
    C64GRP_AMMO_SHELL,
    C64GRP_AMMO_ROCKET,
    C64GRP_AMMO_CELL
} c64_group_t;

static const complex_weighted_t grp_zombiesman[] =
{
    { MT_POSSESSED1, 75 },  
    { MT_POSSESSED2, 20 },
    { MT_CHAINGUY, 5 }
};

static const complex_weighted_t grp_zombiesshotgun[] =
{
    { MT_POSSESSED2, 100 }
};

static const complex_weighted_t grp_imps[] =
{
    { MT_IMP1, 85 },   
    { MT_IMP2, 15 }    
};

static const complex_weighted_t grp_impsnightmare[] =
{
    { MT_IMP2, 100 }
};

static const complex_weighted_t grp_demons[] =
{
    { MT_DEMON1, 55 },
    { MT_DEMON2, 20 },
    { MT_TRITE, 10 },
    { MT_HELLHOUND, 10 },
    { MT_NIGHTMAREDEMON, 5 }
};

static const complex_weighted_t grp_spectres[] =
{
    { MT_DEMON2, 80 },
    { MT_NIGHTMAREDEMON, 20 }
};

static const complex_weighted_t grp_cacodemons[] =
{
    { MT_CACODEMON, 90 },
    { MT_NIGHTMARECACODEMON, 8 },
    { MT_PAIN, 2 }
};

static const complex_weighted_t grp_painelementals[] =
{
    { MT_PAIN, 100 }
};

static const complex_weighted_t grp_baronsofhell[] =
{
    { MT_BRUISER1, 85 },
    { MT_VILE, 15 }
};

static const complex_weighted_t grp_hellknights[] =
{
    { MT_BRUISER2, 70 },
    { MT_UNDEAD, 20 },
    { MT_BRUISER1, 10 }
};

static const complex_weighted_t grp_cyberdemons[] =
{
    { MT_CYBORG, 90 },
    { MT_ANNIHILATOR, 10 }
};

static const complex_weighted_t grp_motherdemons[] =
{
    { MT_RESURRECTOR, 100 }
};

static const complex_weighted_t grp_ammo_clip[] =
{
    { MT_AMMO_CLIP, 80 },
    { MT_AMMO_CLIPBOX, 20 }
};

static const complex_weighted_t grp_ammo_shell[] =
{
    { MT_AMMO_SHELL, 70 },
    { MT_AMMO_SHELLBOX, 30 }
};

static const complex_weighted_t grp_ammo_rocket[] =
{
    { MT_AMMO_ROCKET, 75 },
    { MT_AMMO_ROCKETBOX, 25 }
};

static const complex_weighted_t grp_ammo_cell[] =
{
    { MT_AMMO_CELL, 85 },
    { MT_AMMO_CELLPACK, 15 }
};

static int ComplexD64_AdjustWeight(
    c64_group_t group,
    mobjtype_t type,
    int weight)
{
    int diff = m_complexdoom64_difficulty.value;

    if (diff < C64DIFF_NORMAL || diff > C64DIFF_CHAOS)
        diff = C64DIFF_NORMAL;

    if (weight <= 0)
        return 0;

    switch (diff)
    {
    case C64DIFF_HARDCORE:
        switch (group)
        {
        case C64GRP_ZOMBIEMAN:
            if (type == MT_POSSESSED2 || type == MT_CHAINGUY)
                return weight * 2;
            break;

        case C64GRP_IMP:
            if (type == MT_IMP2)
                return weight * 2;
            break;

        case C64GRP_DEMON:
            if (type == MT_TRITE || type == MT_HELLHOUND || type == MT_NIGHTMAREDEMON)
                return weight * 2;
            break;

        case C64GRP_SPECTRE:
            if (type == MT_NIGHTMAREDEMON)
                return weight * 2;
            break;

        case C64GRP_CACODEMON:
            if (type == MT_NIGHTMARECACODEMON || type == MT_PAIN)
                return weight * 2;
            break;

        case C64GRP_BARON:
            if (type == MT_VILE)
                return weight * 2;
            break;

        case C64GRP_HELLKNIGHT:
            if (type == MT_UNDEAD || type == MT_BRUISER1)
                return weight * 2;
            break;

        case C64GRP_CYBERDEMON:
            if (type == MT_ANNIHILATOR)
                return weight * 2;
            break;
        }
        break;

    case C64DIFF_CHAOS:
        switch (group)
        {
        case C64GRP_ZOMBIEMAN:
            if (type == MT_POSSESSED2 || type == MT_CHAINGUY)
                return weight * 3;
            break;

        case C64GRP_IMP:
            if (type == MT_IMP2)
                return weight * 3;
            break;

        case C64GRP_DEMON:
            if (type == MT_TRITE || type == MT_HELLHOUND || type == MT_NIGHTMAREDEMON)
                return weight * 3;
            break;

        case C64GRP_CACODEMON:
            if (type == MT_NIGHTMARECACODEMON || type == MT_PAIN)
                return weight * 3;
            break;

        case C64GRP_BARON:
            if (type == MT_VILE)
                return weight * 3;
            break;

        case C64GRP_HELLKNIGHT:
            if (type == MT_UNDEAD || type == MT_BRUISER1)
                return weight * 3;
            break;

        case C64GRP_CYBERDEMON:
            if (type == MT_ANNIHILATOR)
                return weight * 3;
            break;
        }
        break;
    }

    return weight;
}

static int ComplexD64_WeightedPick(
    c64_group_t group,
    const complex_weighted_t* list,
    int count)
{
    int weights[16];
    int total = 0;
    int r;
    int i;

    for (i = 0; i < count; i++)
    {
        weights[i] = ComplexD64_AdjustWeight(group, list[i].type, list[i].weight);
        total += weights[i];
    }

    if (total <= 0)
        return list[0].type;

    r = P_Random(pr_complexdoom64randomizer) % total;

    for (i = 0; i < count; i++)
    {
        if (r < weights[i])
            return list[i].type;

        r -= weights[i];
    }

    return list[0].type;
}

mobjtype_t ComplexD64_RandomizeMonster(int type)
{
    if (type <= 0 || type >= NUMMOBJTYPES)
        return type;

    switch (type)
    {
        // ZOMBIES MAN
    case MT_POSSESSED1:
        return ComplexD64_WeightedPick(
            C64GRP_ZOMBIEMAN,
            grp_zombiesman,
            ARRAY_COUNT(grp_zombiesman)
        );

        // ZOMBIES SHOTGUN
    case MT_POSSESSED2:
        return ComplexD64_WeightedPick(
            C64GRP_ZOMBIEMANSHOTGUN,
            grp_zombiesshotgun,
            ARRAY_COUNT(grp_zombiesshotgun)
        );

        // IMPS
    case MT_IMP1:
        return ComplexD64_WeightedPick(
            C64GRP_IMP,
            grp_imps,
            ARRAY_COUNT(grp_imps)
        );

        // IMPS NIGHTMARE
    case MT_IMP2:
        return ComplexD64_WeightedPick(
            C64GRP_IMPNIGHTMARE,
            grp_impsnightmare,
            ARRAY_COUNT(grp_impsnightmare)
        );

        // DEMONS
    case MT_DEMON1:
        return ComplexD64_WeightedPick(
            C64GRP_DEMON,
            grp_demons,
            ARRAY_COUNT(grp_demons)
        );

        // SPECTRES
    case MT_DEMON2:
        return ComplexD64_WeightedPick(
            C64GRP_SPECTRE,
            grp_spectres,
            ARRAY_COUNT(grp_spectres)
        );

        // CACODEMONS
    case MT_CACODEMON:
        return ComplexD64_WeightedPick(
            C64GRP_CACODEMON,
            grp_cacodemons,
            ARRAY_COUNT(grp_cacodemons)
        );

        // PAIN ELEMENTALS
    case MT_PAIN:
        return ComplexD64_WeightedPick(
            C64GRP_PAINELEMENTAL,
            grp_painelementals,
            ARRAY_COUNT(grp_painelementals)
        );

        // BARONS OF HELL
    case MT_BRUISER1:
        return ComplexD64_WeightedPick(
            C64GRP_BARON,
            grp_baronsofhell,
            ARRAY_COUNT(grp_baronsofhell)
        );

        // HELL KNIGHTS
    case MT_BRUISER2:
        return ComplexD64_WeightedPick(
            C64GRP_HELLKNIGHT,
            grp_hellknights,
            ARRAY_COUNT(grp_hellknights)
        );

        // CYBERDEMONS
    case MT_CYBORG:
        return ComplexD64_WeightedPick(
            C64GRP_CYBERDEMON,
            grp_cyberdemons,
            ARRAY_COUNT(grp_cyberdemons)
        );

        // MOTHER DEMONS
    case MT_RESURRECTOR:
        return ComplexD64_WeightedPick(
            C64GRP_MOTHERDEMON,
            grp_motherdemons,
            ARRAY_COUNT(grp_motherdemons)
        );

        // CLIPS
    case MT_AMMO_CLIP:
        return ComplexD64_WeightedPick(
            C64GRP_AMMO_CLIP,
            grp_ammo_clip,
            ARRAY_COUNT(grp_ammo_clip)
        );

        // SHELLS
    case MT_AMMO_SHELL:
        return ComplexD64_WeightedPick(
            C64GRP_AMMO_SHELL,
            grp_ammo_shell,
            ARRAY_COUNT(grp_ammo_shell)
        );

        // ROCKETS
    case MT_AMMO_ROCKET:
        return ComplexD64_WeightedPick(
            C64GRP_AMMO_ROCKET,
            grp_ammo_rocket,
            ARRAY_COUNT(grp_ammo_rocket)
        );

        // CELLS
    case MT_AMMO_CELL:
        return ComplexD64_WeightedPick(
            C64GRP_AMMO_CELL,
            grp_ammo_cell,
            ARRAY_COUNT(grp_ammo_cell)
        );

    default:
        return type;
    }
}
