// tutorial.c -- see tutorial.h for the shape of the thing.
//
// The whole system is a bitmask and one lookup: a step's hint is offered only
// while every earlier step is done-and-marked, so the player gets exactly one
// sentence at a time and always one they can act on right now. Nothing here
// pauses the game, spawns a dialog or demands a confirmation -- the chain
// teaches by being there, not by getting in the way.
//
// The chain now covers more than the hub loop. The field beats (TUT_B_*) are
// one-shot toasts fired by game.c at the exact moment the thing happens --
// first hit taken, ultimate first charged -- via tutorial_mark(); the two
// modals (duel card, prologue) are acknowledged once and never again. All of
// it is bits in the same saved word, so a veteran's file silences everything
// on boot exactly as it always did.

#include "tutorial.h"
#include "farm.h"
#include "hub_layout.h"

#define TUT_BIT(step)  (1u << (step))

static bool tut_done(int step)
{
    return (g_tut_flags & TUT_BIT(step)) != 0;
}

static void tut_mark(int step)
{
    if (step >= 0 && step < TUT_COUNT)
        g_tut_flags |= TUT_BIT(step);
}

void tutorial_mark(int step)
{
    tut_mark(step);
}

bool tutorial_done(int step)
{
    return tut_done(step);
}

// A save that already shows progress belongs to someone who knows the loop.
// Called once at boot, after the save has been read: any planted plot, any
// day over the first, any secured ingredient marks the entire chain done and
// the system goes quiet for that file's lifetime.
void tutorial_boot(void)
{
    bool veteran = (g_day > 1) || (g_ingredients_done != 0);
    if (!veteran)
    {
        for (int i = 0; i < FARM_PLOTS; i++)
        {
            if (g_farm[i].crop)
            {
                veteran = true;
                break;
            }
        }
    }
    if (veteran)
    {
        for (int i = 0; i < TUT_COUNT; i++)
            tut_mark(i);
    }
}

// An action happened somewhere in the game; advance the chain from it.
// Every call is a no-op once its step is marked, so call sites do not have
// to check anything.
void tutorial_notify(int event)
{
    switch (event)
    {
    case TUT_EV_REGION_ENTER: tut_mark(TUT_GATE);   break;
    case TUT_EV_SEED_PICKED:  tut_mark(TUT_SEED);   break;
    case TUT_EV_PLANTED:      tut_mark(TUT_PLANT);  break;
    case TUT_EV_WATERED:      tut_mark(TUT_WATER);  break;
    case TUT_EV_SLEPT:        tut_mark(TUT_SLEEP);  break;
    case TUT_EV_HARVESTED:    tut_mark(TUT_HARVEST); break;
    case TUT_EV_OVEN:         tut_mark(TUT_OVEN);   break;
    default: break;
    }
}

// The one line the hub should be showing right now, or NULL when there is
// nothing to teach (which is the steady state for anyone but a first-timer).
// Steps before PLANT have no hub line: the gate speaks for itself, and the
// seed moment is a field toast at the pickup, not a hint to remember.
// Directions are anchored to what the place looks like -- "the tallest gate",
// "the steaming pool" -- because a newcomer has no map in their head yet;
// where words would fail, the edge arrow (tutorial_arrow_landmark) points.
const char *tutorial_hint(void)
{
    if (!tut_done(TUT_GATE))
        return "跟着箭头，到最高的那座门按 A";

    if (!tut_done(TUT_PLANT))
    {
        if (!tut_done(TUT_SEED))
            return NULL;
        return (farm_seed_total() > 0)
            ? "跟着箭头去菜地，按 A 播种"
            : "种子没了？回那座门再打一趟";
    }

    if (!tut_done(TUT_WATER))
        return "种下了就浇水：站在菜地里按 B";

    if (!tut_done(TUT_SLEEP))
        return "冒着热气的温泉按 A：睡一觉作物长大";

    if (!tut_done(TUT_HARVEST))
    {
        if (farm_ripe_count() > 0)
            return "有作物成熟了：走近菜地按 A 收获";
        return "睡觉两次后作物就熟了，回菜地收获材料";
    }

    if (!tut_done(TUT_OVEN))
    {
        if (cake_ready())
        {
            tut_mark(TUT_OVEN);
            return "材料齐了！去南边的烤箱烤蛋糕吧";
        }
        return "材料记进烤箱的账：集齐五种就能烤蛋糕";
    }

    return NULL;
}

// Where the hub's edge arrow should point while the chain is talking: the
// landmark the current hint is about. The field gets HUB_TUT_FARM; 0 means
// nothing to point at right now. Worded hints and arrows disagree on their
// own and the arrow wins -- a pointing arrow with a stale sentence still
// gets the player there.
int tutorial_arrow_landmark(void)
{
    if (!tut_done(TUT_GATE))
        return HUB_LM_GATE_ID;

    if (!tut_done(TUT_PLANT))
    {
        if (!tut_done(TUT_SEED))
            return 0;
        return (farm_seed_total() > 0) ? HUB_TUT_FARM : HUB_LM_GATE_ID;
    }

    if (!tut_done(TUT_WATER) || !tut_done(TUT_HARVEST))
        return HUB_TUT_FARM;

    if (!tut_done(TUT_SLEEP))
        return HUB_LM_SPRING_ID;

    if (!tut_done(TUT_OVEN) && cake_ready())
        return HUB_LM_OVEN_ID;

    return 0;
}
