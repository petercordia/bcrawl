/**
 * @file player_reacts.cc
 * @brief Player functions called every turn, mostly handling enchantment durations/expirations.
 **/

#include "AppHdr.h"

#include "player-reacts.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <sstream>
#include <string>

#ifndef TARGET_OS_WINDOWS
# ifndef __ANDROID__
#  include <langinfo.h>
# endif
#endif
#include <fcntl.h>
#ifdef USE_UNIX_SIGNALS
#include <csignal>
#endif

#include "abyss.h" // abyss_maybe_spawn_xp_exit
#include "act-iter.h"
#include "areas.h"
#include "artefact.h"
#include "beam.h"
#include "cloud.h"
#include "clua.h"
#include "colour.h"
#include "coord.h"
#include "coordit.h"
#include "database.h"
#include "dbg-util.h"
#include "delay.h"
#ifdef DGL_SIMPLE_MESSAGING
#include "dgl-message.h"
#endif
#include "dlua.h"
#include "dungeon.h"
#include "env.h"
#include "exercise.h"
#include "files.h"
#include "food.h"
#include "god-abil.h"
#include "god-companions.h"
#include "god-passive.h"
#include "invent.h"
#include "item-prop.h"
#include "item-use.h"
#include "level-state-type.h"
#include "libutil.h"
#include "maps.h"
#include "message.h"
#include "mon-abil.h"
#include "mon-cast.h"
#include "mon-death.h"
#include "mon-place.h"
#include "mon-tentacle.h"
#include "mon-util.h"
#include "mutation.h"
#include "ouch.h"
#include "player.h"
#include "player-stats.h"
#include "random.h"
#include "religion.h"
#include "shopping.h"
#include "shout.h"
#include "skills.h"
#include "spl-cast.h"
#include "spl-clouds.h"
#include "spl-damage.h"
#include "spl-goditem.h"
#include "spl-other.h"
#include "spl-summoning.h"
#include "spl-transloc.h"
#include "spl-util.h"
#include "state.h"
#include "status.h"
#include "stepdown.h"
#include "stringutil.h"
#include "terrain.h"
#ifdef USE_TILE
#include "tiledef-dngn.h"
#include "tilepick.h"
#endif
#include "transform.h"
#include "traps.h"
#include "travel.h"
#include "view.h"
#include "xom.h"

/**
 * Decrement a duration by the given delay.

 * The midloss value should be either 0 or a number of turns where the delay
 * from those turns at normal speed is less than the duration's midpoint. The
 * use of midloss prevents the player from knowing the exact remaining duration
 * when the midpoint message is displayed.
 *
 * @param dur The duration type to be decremented.
 * @param delay The delay aut amount by which to decrement the duration.
 * @param endmsg The message to be displayed when the duration ends.
 * @param midloss A number of normal-speed turns by which to further decrement
 *                the duration if we cross the duration's midpoint.
 * @param endmsg The message to be displayed when the duration is decremented
 *               to a value under its midpoint.
 * @param chan The channel where the endmsg will be printed if the duration
 *             ends.
 *
 * @return  True if the duration ended, false otherwise.
 */

static bool _decrement_a_duration(duration_type dur, int delay,
                                 const char* endmsg = nullptr,
                                 int midloss = 0,
                                 const char* midmsg = nullptr,
                                 msg_channel_type chan = MSGCH_DURATION)
{
    ASSERT(you.duration[dur] >= 0);
    if (you.duration[dur] == 0)
        return false;

    ASSERT(!midloss || midmsg != nullptr);
    const int midpoint = duration_expire_point(dur);
    ASSERTM(!midloss || midloss * BASELINE_DELAY < midpoint,
            "midpoint delay loss %d not less than duration midpoint %d",
            midloss * BASELINE_DELAY, midpoint);

    const int old_dur = you.duration[dur];
    you.duration[dur] -= delay;

    // If we cross the midpoint, handle midloss and print the midpoint message.
    if (you.duration[dur] <= midpoint && old_dur > midpoint)
    {
        you.duration[dur] -= midloss * BASELINE_DELAY;
        if (midmsg)
        {
            // Make sure the player has a turn to react to the midpoint
            // message.
            if (you.duration[dur] <= 0)
                you.duration[dur] = 1;
            if (need_expiration_warning(dur))
                mprf(MSGCH_DANGER, "Careful! %s", midmsg);
            else
                mprf(chan, "%s", midmsg);
        }
    }

    if (you.duration[dur] <= 0)
    {
        you.duration[dur] = 0;
        if (endmsg && *endmsg != '\0')
            mprf(chan, "%s", endmsg);
        return true;
    }

    return false;
}


static void _decrement_petrification(int delay)
{
    if (_decrement_a_duration(DUR_PETRIFIED, delay))
    {
        you.redraw_evasion = true;

        string flesh_equiv = "flesh";
        if(!get_form()->flesh_equivalent.empty())
            flesh_equiv = get_form()->flesh_equivalent;
        else
            switch (you.species)
            {
            case SP_SKELETON: flesh_equiv = "bones"; break;
            case SP_ENT: flesh_equiv = "wood"; break;
            default: break;
            }

        mprf(MSGCH_DURATION, "You turn to %s%s.",
             flesh_equiv.c_str(),
             you.paralysed() ? "" : " and can move again");

        if (you.props.exists(PETRIFIED_BY_KEY))
            you.props.erase(PETRIFIED_BY_KEY);
    }

    if (you.duration[DUR_PETRIFYING])
    {
        int &dur = you.duration[DUR_PETRIFYING];
        int old_dur = dur;
        if ((dur -= delay) <= 0)
        {
            dur = 0;
            you.fully_petrify(nullptr);
        }
        else if (dur < 15 && old_dur >= 15)
            mpr("Your limbs are stiffening.");
    }
}

static void _decrement_paralysis(int delay)
{
    _decrement_a_duration(DUR_PARALYSIS_IMMUNITY, delay);

    if (you.duration[DUR_PARALYSIS])
    {
        _decrement_a_duration(DUR_PARALYSIS, delay);

        if (!you.duration[DUR_PARALYSIS] && !you.petrified())
        {
            mprf(MSGCH_DURATION, "You can move again.");
            you.redraw_evasion = true;
            you.duration[DUR_PARALYSIS_IMMUNITY] = roll_dice(1, 3)
            * BASELINE_DELAY;
            if (you.props.exists(PARALYSED_BY_KEY))
                you.props.erase(PARALYSED_BY_KEY);
        }
    }
}

/**
 * Check whether the player's ice (Ozocubu's) armour was melted this turn.
 * If so, print the appropriate message and clear the flag.
 */
static void _maybe_melt_armour()
{
    // We have to do the messaging here, because a simple wand of flame will
    // call _maybe_melt_player_enchantments twice. It also avoids duplicate
    // messages when melting because of several heat sources.
    if (you.props.exists(MELT_ARMOUR_KEY))
    {
        you.props.erase(MELT_ARMOUR_KEY);
        mprf(MSGCH_DURATION, "The heat melts your icy armour.");
    }
}

/**
 * How much horror does the player character feel in the current situation?
 *
 * (For Ru's MUT_COWARDICE.)
 *
 * Penalties are based on the "scariness" (threat level) of monsters currently
 * visible.
 */
static int _current_horror_level()
{
    int horror_level = 0;

    for (monster_near_iterator mi(&you, LOS_NO_TRANS); mi; ++mi)
    {

        if (mons_aligned(*mi, &you)
            || !mons_is_threatening(**mi)
            || mons_is_tentacle_or_tentacle_segment(mi->type))
        {
            continue;
        }

        const mon_threat_level_type threat_level = mons_threat_level(**mi);
        if (threat_level == MTHRT_NASTY)
            horror_level += 3;
        else if (threat_level == MTHRT_TOUGH)
            horror_level += 1;
    }
    // Subtract one from the horror level so that you don't get a message
    // when a single tough monster appears.
    horror_level = max(0, horror_level - 1);
    return horror_level;
}

/**
 * What was the player's most recent horror level?
 *
 * (For Ru's MUT_COWARDICE.)
 */
static int _old_horror_level()
{
    if (you.duration[DUR_HORROR])
        return you.props[HORROR_PENALTY_KEY].get_int();
    return 0;
}

/**
 * When the player should no longer be horrified, end the DUR_HORROR if it
 * exists & cleanup the corresponding prop.
 */
static void _end_horror()
{
    if (!you.duration[DUR_HORROR])
        return;

    you.props.erase(HORROR_PENALTY_KEY);
    you.set_duration(DUR_HORROR, 0);
}

/**
 * Update penalties for cowardice based on the current situation, if the player
 * has Ru's MUT_COWARDICE.
 */
static void _update_cowardice()
{
    if (!you.has_mutation(MUT_COWARDICE))
    {
        // If the player somehow becomes sane again, handle that
        _end_horror();
        return;
    }

    const int horror_level = _current_horror_level();

    if (horror_level <= 0)
    {
        // If you were horrified before & aren't now, clean up.
        _end_horror();
        return;
    }

    // Lookup the old value before modifying it
    const int old_horror_level = _old_horror_level();

    // as long as there's still scary enemies, keep the horror going
    you.props[HORROR_PENALTY_KEY] = horror_level;
    you.set_duration(DUR_HORROR, 1);

    // only show a message on increase
    if (horror_level <= old_horror_level)
        return;

    if (horror_level >= HORROR_LVL_OVERWHELMING)
        mpr("Monsters! Monsters everywhere! You have to get out of here!");
    else if (horror_level >= HORROR_LVL_EXTREME)
        mpr("You reel with horror at the sight of these foes!");
    else
        mpr("You feel a twist of horror at the sight of this foe.");
}

// Uskawyaw piety decays incredibly fast, but only to a baseline level of *.
// Using Uskayaw abilities can still take you under *.
static void _handle_uskayaw_piety(int time_taken)
{
    if (you.props[USKAYAW_NUM_MONSTERS_HURT].get_int() > 0)
    {
        int num_hurt = you.props[USKAYAW_NUM_MONSTERS_HURT];
        int hurt_val = you.props[USKAYAW_MONSTER_HURT_VALUE];
        int piety_gain = max(num_hurt, stepdown_value(hurt_val, 5, 10, 20, 40));

        gain_piety(piety_gain);
        you.props[USKAYAW_AUT_SINCE_PIETY_GAIN] = 0;

        you.props[USKAYAW_AUDIENCE_TIMER] = max(0, you.props[USKAYAW_AUDIENCE_TIMER].get_int() - piety_gain);
    }
    else if (you.piety > piety_breakpoint(0))
    {
        // exponential piety loss
        int exp_loss = div_rand_round(piety_scale(you.piety), 25);
        exp_loss = min(exp_loss, you.piety - piety_breakpoint(0));
        if (exp_loss > 0)
            lose_piety(exp_loss);

        // no-combat piety loss
        int time_since_gain = you.props[USKAYAW_AUT_SINCE_PIETY_GAIN].get_int();
        time_since_gain += time_taken;

        // Only start losing piety if it's been a few turns since we gained
        // piety, in order to give more tolerance for missing in combat.
        if (time_since_gain > 30)
        {
            int piety_lost = min(you.piety - piety_breakpoint(0),
                    div_rand_round(time_since_gain, 20));

            if (piety_lost > 0)
                lose_piety(piety_lost);

        }
        you.props[USKAYAW_AUT_SINCE_PIETY_GAIN] = time_since_gain;
    }
    else  // resting should reset timer without extra waiting
        you.props[USKAYAW_AUDIENCE_TIMER] = 0;

    // Re-initialize Uskayaw piety variables
    you.props[USKAYAW_NUM_MONSTERS_HURT] = 0;
    you.props[USKAYAW_MONSTER_HURT_VALUE] = 0;
}

static void _handle_uskayaw_time(int time_taken)
{
    _handle_uskayaw_piety(time_taken);

    int audience_timer = you.props[USKAYAW_AUDIENCE_TIMER].get_int();

    // For the timered abilities, if we set the timer to -1, that means we
    // need to trigger the abilities this turn. Otherwise we'll decrement the
    // timer down to a minimum of 0, at which point it becomes eligible to
    // trigger again.
    if (audience_timer == -1
            || (you.piety >= piety_breakpoint(3) && audience_timer <= time_taken))
    {
        uskayaw_prepares_audience();
    }
    else
        you.props[USKAYAW_AUDIENCE_TIMER] = max(0, audience_timer - time_taken);
}

/**
 * Player reactions after monster and cloud activities in the turn are finished.
 */
void player_reacts_to_monsters()
{
    // In case Maurice managed to steal a needed item for example.
    if (!you_are_delayed())
        update_can_train();

    if (you.duration[DUR_FIRE_SHIELD] > 0)
        manage_fire_shield(you.time_taken);

    check_monster_detect();

    if (have_passive(passive_t::detect_items) || you.has_mutation(MUT_JELLY_GROWTH)
        || you.get_mutation_level(MUT_STRONG_NOSE) > 0)
    {
        detect_items(-1);
    }

    _decrement_paralysis(you.time_taken);
    _decrement_petrification(you.time_taken);
    if (_decrement_a_duration(DUR_SLEEP, you.time_taken))
        you.awaken();

    if (_decrement_a_duration(DUR_GRASPING_ROOTS, you.time_taken)
        && you.is_constricted())
    {
        // We handle the end-of-enchantment message here since the method
        // of constriction is no longer detectable.
        mprf("The grasping roots release their grip on you.");
        you.stop_being_constricted(true);
    }

    _maybe_melt_armour();
    _update_cowardice();
    if (you_worship(GOD_USKAYAW))
        _handle_uskayaw_time(you.time_taken);
}

static bool _check_recite()
{
    if (silenced(you.pos())
        || you.paralysed()
        || you.confused()
        || you.asleep()
        || you.petrified()
        || you.berserk())
    {
        mprf(MSGCH_DURATION, "Your recitation is interrupted.");
        you.duration[DUR_RECITE] = 0;
        you.set_duration(DUR_RECITE_COOLDOWN, 1 + random2(10) + random2(30));
        return false;
    }
    return true;
}


static void _handle_recitation(int step)
{
    mprf("\"%s\"",
         zin_recite_text(you.attribute[ATTR_RECITE_SEED],
                         you.attribute[ATTR_RECITE_TYPE], step).c_str());

    if (apply_area_visible(zin_recite_to_single_monster, you.pos()))
        viewwindow();

    // Recite trains more than once per use, because it has a
    // long timer in between uses and actually takes up multiple
    // turns.
    practise_using_ability(ABIL_ZIN_RECITE);

    noisy(you.shout_volume(), you.pos());

    if (step == 0)
    {
        ostringstream speech;
        speech << zin_recite_text(you.attribute[ATTR_RECITE_SEED],
                                  you.attribute[ATTR_RECITE_TYPE], -1);
        speech << '.';
        if (one_chance_in(27))
        {
            const string closure = getSpeakString("recite_closure");
            if (!closure.empty())
                speech << ' ' << closure;
        }
        mprf(MSGCH_DURATION, "You finish reciting %s", speech.str().c_str());
        you.set_duration(DUR_RECITE_COOLDOWN, 1 + random2(10) + random2(30));
    }
}

/**
 * Take a 'simple' duration, decrement it, and print messages as appropriate
 * when it hits 50% and 0% remaining.
 *
 * @param dur       The duration in question.
 * @param delay     How much to decrement the duration by.
 */
static void _decrement_simple_duration(duration_type dur, int delay)
{
    if (_decrement_a_duration(dur, delay, duration_end_message(dur),
                             duration_mid_offset(dur),
                             duration_mid_message(dur),
                             duration_mid_chan(dur)))
    {
        duration_end_effect(dur);
    }
}



/**
 * Decrement player durations based on how long the player's turn lasted in aut.
 */
static void _decrement_durations()
{
    const int delay = you.time_taken;

    if (you.gourmand())
    {
        // Innate gourmand is always fully active.
        if (you.has_mutation(MUT_GOURMAND))
            you.duration[DUR_GOURMAND] = GOURMAND_MAX;
        else if (you.duration[DUR_GOURMAND] < GOURMAND_MAX && coinflip())
            you.duration[DUR_GOURMAND] += delay;
    }
    else
        you.duration[DUR_GOURMAND] = 0;

    if (you.duration[DUR_LIQUID_FLAMES])
        dec_napalm_player(delay);

    const bool melted = you.props.exists(MELT_ARMOUR_KEY);
    if (_decrement_a_duration(DUR_ICY_ARMOUR, delay,
                              "Your icy armour evaporates.",
                              melted ? 0 : coinflip(),
                              melted ? nullptr
                              : "Your icy armour starts to melt."))
    {
        if (you.props.exists(ICY_ARMOUR_KEY))
            you.props.erase(ICY_ARMOUR_KEY);
        you.redraw_armour_class = true;
    }

    // Possible reduction of silence radius.
    if (you.duration[DUR_SILENCE])
        invalidate_agrid();
    // and liquefying radius.
    if (you.duration[DUR_LIQUEFYING])
        invalidate_agrid();

    // FIXME: [ds] Remove this once we've ensured durations can never go < 0?
    if (you.duration[DUR_TRANSFORMATION] <= 0
        && you.form != transformation::none)
    {
        you.duration[DUR_TRANSFORMATION] = 1;
    }

    // Vampire bat transformations are permanent (until ended), unless they
    // are uncancellable (polymorph wand on a full vampire).
    if (you.species != SP_VAMPIRE || you.form != transformation::bat
        || you.duration[DUR_TRANSFORMATION] <= 5 * BASELINE_DELAY
        || you.transform_uncancellable)
    {
        if (form_can_fly()
            || form_likes_water() && feat_is_water(grd(you.pos())))
        {
            // Disable emergency flight if it was active
            you.props.erase(EMERGENCY_FLIGHT_KEY);
        }

        bool last_forever = false;
        switch(you.form)
        {
        case transformation::spider:
            last_forever = you.has_spell(SPELL_SPIDER_FORM) && calc_spell_power(SPELL_SPIDER_FORM, true) >= 15;
            break;
        case transformation::ice_beast:
            last_forever = you.has_spell(SPELL_ICE_FORM) && calc_spell_power(SPELL_ICE_FORM, true) >= 25;
            break;
        case transformation::statue:
            last_forever = you.has_spell(SPELL_STATUE_FORM) && calc_spell_power(SPELL_STATUE_FORM, true) >= 50;
            break;
        case transformation::dragon:
            last_forever = you.has_spell(SPELL_DRAGON_FORM) && calc_spell_power(SPELL_DRAGON_FORM, true) >= 75;
            break;
        case transformation::scorpion:
            last_forever = you.has_spell(SPELL_SCORPION_FORM) && calc_spell_power(SPELL_SCORPION_FORM, true) >= 75;
            break;
        case transformation::lich:
            last_forever = you.has_spell(SPELL_NECROMUTATION) && calc_spell_power(SPELL_NECROMUTATION, true) >= 100;
            break;
        default: break;
        }
        if(!last_forever || you.transform_uncancellable)
            if (_decrement_a_duration(DUR_TRANSFORMATION, delay, nullptr,
                    random2(3), "Your transformation is almost over."))
                untransform();
    }

    if (you.attribute[ATTR_SWIFTNESS] >= 0)
    {
        if (_decrement_a_duration(DUR_SWIFTNESS, delay,
                                  "You feel sluggish.", coinflip(),
                                  "You start to feel a little slower."))
        {
            // Start anti-swiftness.
            you.duration[DUR_SWIFTNESS] = you.attribute[ATTR_SWIFTNESS];
            you.attribute[ATTR_SWIFTNESS] = -1;
        }
    }
    else
    {
        if (_decrement_a_duration(DUR_SWIFTNESS, delay,
                                  "You no longer feel sluggish.", coinflip(),
                                  "You start to feel a little faster."))
        {
            you.attribute[ATTR_SWIFTNESS] = 0;
        }
    }

    // Decrement Powered By Death strength
    int pbd_str = you.props[POWERED_BY_DEATH_KEY].get_int();
    if (pbd_str > 0 && _decrement_a_duration(DUR_POWERED_BY_DEATH, delay))
    {
        you.props[POWERED_BY_DEATH_KEY] = pbd_str - 1;
        reset_powered_by_death_duration();
    }

    dec_ambrosia_player(delay);
    dec_channel_player(delay);
    dec_slow_player(delay);
    dec_berserk_recovery_player(delay);
    dec_haste_player(delay);

    if (you.duration[DUR_LIQUEFYING] && !you.stand_on_solid_ground())
        you.duration[DUR_LIQUEFYING] = 1;

    for (int i = 0; i < NUM_STATS; ++i)
    {
        stat_type s = static_cast<stat_type>(i);
        if (you.stat(s) > 0
            && _decrement_a_duration(stat_zero_duration(s), delay))
        {
            mprf(MSGCH_RECOVERY, "Your %s has recovered.", stat_desc(s, SD_NAME));
            you.redraw_stats[s] = true;
            if (you.duration[DUR_SLOW] == 0)
                mprf(MSGCH_DURATION, "You feel yourself speed up.");
        }
    }

    // Leak piety from the piety pool into actual piety.
    // Note that changes of religious status without corresponding actions
    // (killing monsters, offering items, ...) might be confusing for characters
    // of other religions.
    // For now, though, keep information about what happened hidden.
    if (you.piety < MAX_PIETY && you.duration[DUR_PIETY_POOL] > 0
        && one_chance_in(5))
    {
        you.duration[DUR_PIETY_POOL]--;
        gain_piety(1, 1);

#if defined(DEBUG_DIAGNOSTICS) || defined(DEBUG_SACRIFICE) || defined(DEBUG_PIETY)
        mprf(MSGCH_DIAGNOSTICS, "Piety increases by 1 due to piety pool.");

        if (you.duration[DUR_PIETY_POOL] == 0)
            mprf(MSGCH_DIAGNOSTICS, "Piety pool is now empty.");
#endif
    }

    // Should expire before flight.
    if (you.duration[DUR_TORNADO])
    {
        tornado_damage(&you, min(delay, you.duration[DUR_TORNADO]));
        if (_decrement_a_duration(DUR_TORNADO, delay,
                                  "The winds around you start to calm down."))
        {
            you.duration[DUR_TORNADO_COOLDOWN] = random_range(55, 65);
        }
    }

    if (you.duration[DUR_FLIGHT])
    {
        if (!you.permanent_flight())
        {
            if (_decrement_a_duration(DUR_FLIGHT, delay, nullptr, random2(6),
                                      "You are starting to lose your buoyancy."))
            {
                land_player();
            }
            else
            {
                // Disable emergency flight if it was active
                you.props.erase(EMERGENCY_FLIGHT_KEY);
            }
        }
        else if ((you.duration[DUR_FLIGHT] -= delay) <= 0)
        {
            // Just time out potions/spells/miscasts.
            you.attribute[ATTR_FLIGHT_UNCANCELLABLE] = 0;
            you.duration[DUR_FLIGHT] = 0;
            you.props.erase(EMERGENCY_FLIGHT_KEY);
        }
    }

    if (_decrement_a_duration(DUR_CLOUD_TRAIL, delay,
            "Your trail of clouds dissipates."))
    {
        you.props.erase(XOM_CLOUD_TRAIL_TYPE_KEY);
    }

    if (you.duration[DUR_DARKNESS] && you.haloed())
    {
        you.duration[DUR_DARKNESS] = 0;
        mpr("The divine light dispels your darkness!");
        update_vision_range();
    }

    if (you.duration[DUR_WATER_HOLD])
        handle_player_drowning(delay);

    if (you.duration[DUR_FLAYED])
    {
        bool near_ghost = false;
        for (monster_iterator mi; mi; ++mi)
        {
            if (mi->type == MONS_FLAYED_GHOST && !mi->wont_attack()
                && you.see_cell(mi->pos()))
            {
                near_ghost = true;
                break;
            }
        }
        if (!near_ghost)
        {
            if (_decrement_a_duration(DUR_FLAYED, delay))
                heal_flayed_effect(&you);
        }
        else if (you.duration[DUR_FLAYED] < 80)
            you.duration[DUR_FLAYED] += div_rand_round(50, delay);
    }

    if (you.duration[DUR_TOXIC_RADIANCE])
        toxic_radiance_effect(&you, min(delay, you.duration[DUR_TOXIC_RADIANCE]));

    if (you.duration[DUR_RECITE] && _check_recite())
    {
        const int old_recite =
            (you.duration[DUR_RECITE] + BASELINE_DELAY - 1) / BASELINE_DELAY;
        _decrement_a_duration(DUR_RECITE, delay);
        const int new_recite =
            (you.duration[DUR_RECITE] + BASELINE_DELAY - 1) / BASELINE_DELAY;
        if (old_recite != new_recite)
            _handle_recitation(new_recite);
    }

    if (you.attribute[ATTR_NEXT_RECALL_INDEX] > 0)
        do_recall(delay);

    if (you.duration[DUR_DRAGON_CALL])
        do_dragon_call(delay);

    if (you.duration[DUR_ABJURATION_AURA])
        do_aura_of_abjuration(delay);

    if (you.duration[DUR_DOOM_HOWL])
        doom_howl(min(delay, you.duration[DUR_DOOM_HOWL]));

    dec_elixir_player(delay);

    if (!you.cannot_move()
        && !you.confused()
        && !you.asleep())
    {
        extract_manticore_spikes(
            make_stringf("You %s the barbed spikes from your body.",
                you.berserk() ? "rip and tear" : "carefully extract").c_str());
    }

    if (!env.sunlight.empty())
        process_sunlights();

    if (!you.duration[DUR_ANCESTOR_DELAY]
        && in_good_standing(GOD_HEPLIAKLQANA)
        && hepliaklqana_ancestor() == MID_NOBODY)
    {
        try_respawn_ancestor();
    }

    const bool sanguine_armour_is_valid = sanguine_armour_valid();
    if (sanguine_armour_is_valid)
        activate_sanguine_armour();
    else if (!sanguine_armour_is_valid && you.duration[DUR_SANGUINE_ARMOUR])
        you.duration[DUR_SANGUINE_ARMOUR] = 1; // expire

    if (you.attribute[ATTR_HEAVENLY_STORM]
        && !you.duration[DUR_HEAVENLY_STORM])
    {
        end_heavenly_storm(); // we shouldn't hit this, but just in case
    }

    // these should be after decr_ambrosia, transforms, liquefying, etc.
    for (int i = 0; i < NUM_DURATIONS; ++i)
        if (duration_decrements_normally((duration_type) i))
            _decrement_simple_duration((duration_type) i, delay);
}

static void _handle_emergency_flight()
{
    ASSERT(you.props[EMERGENCY_FLIGHT_KEY].get_bool());

    if (!is_feat_dangerous(orig_terrain(you.pos()), true, false))
    {
        mpr("You float gracefully downwards.");
        land_player();
        you.props.erase(EMERGENCY_FLIGHT_KEY);
    }
    else
    {
        const int drain = div_rand_round(15 * you.time_taken, BASELINE_DELAY);
        drain_player(drain, true, true);
    }
}

// cjo: Handles player hp and mp regeneration. If the counter
// you.hit_points_regeneration is over 100, a loop restores 1 hp and decreases
// the counter by 100 (so you can regen more than 1 hp per turn). If the counter
// is below 100, it is increased by a variable calculated from delay,
// BASELINE_DELAY, and your regeneration rate. MP regeneration happens
// similarly, but the countup depends on delay, BASELINE_DELAY, and
// you.max_magic_points
void regenerate_hp_and_mp(int delay, bool apply_bonuses)
{
    if (crawl_state.disables[DIS_PLAYER_REGEN])
        return;

    // HP Regeneration
    if (!you.duration[DUR_DEATHS_DOOR])
    {
        const int base_val = player_regen(apply_bonuses);
        you.hit_points_regeneration += div_rand_round(base_val * delay, BASELINE_DELAY);
    }

    while (you.hit_points_regeneration >= 100)
    {
        // at low mp, "mana link" restores mp in place of hp
        if (you.has_mutation(MUT_MANA_LINK)
            && !x_chance_in_y(you.magic_points, you.max_magic_points))
        {
            inc_mp(1);
        }
        else // standard hp regeneration
            inc_hp(1);
        you.hit_points_regeneration -= 100;
    }

    ASSERT_RANGE(you.hit_points_regeneration, 0, 100);

    update_amulet_attunement_by_health();

    // MP Regeneration
    if (!player_regenerates_mp())
        return;

    if (you.magic_points < you.max_magic_points)
    {
        const int base_val = player_mp_regen();
        int mp_regen_countup = div_rand_round(base_val * delay, BASELINE_DELAY);
        you.magic_points_regeneration += mp_regen_countup;
    }

    while (you.magic_points_regeneration >= 100)
    {
        inc_mp(1);
        you.magic_points_regeneration -= 100;
    }

    ASSERT_RANGE(you.magic_points_regeneration, 0, 100);

    update_mana_regen_amulet_attunement();
}

void player_reacts()
{
    //XXX: does this _need_ to be calculated up here?
    const int stealth = player_stealth();

#ifdef DEBUG_STEALTH
    // Too annoying for regular diagnostics.
    mprf(MSGCH_DIAGNOSTICS, "stealth: %d", stealth);
#endif

    if (you.has_mutation(MUT_DEMONIC_GUARDIAN))
        check_demonic_guardian();

    if (you.unrand_reacts.any())
        unrand_reacts();

    // Handle sound-dependent effects that are silenced
    if (silenced(you.pos()))
    {
        if (you.duration[DUR_SONG_OF_SLAYING])
        {
            mpr("The silence causes your song to end.");
            _decrement_a_duration(DUR_SONG_OF_SLAYING, you.duration[DUR_SONG_OF_SLAYING]);
        }
    }

    // Singing makes a continuous noise
    if (you.duration[DUR_SONG_OF_SLAYING])
        noisy(spell_effect_noise(SPELL_SONG_OF_SLAYING), you.pos());

    if (x_chance_in_y(you.time_taken, 10 * BASELINE_DELAY))
    {
        const int teleportitis_level = player_teleport();
        // this is instantaneous
        if (teleportitis_level > 0 && one_chance_in(100 / teleportitis_level))
            you_teleport_now(false, true);
        else if (player_in_branch(BRANCH_ABYSS) && one_chance_in(80)
                 && (!map_masked(you.pos(), MMT_VAULT) || one_chance_in(3)))
        {
            you_teleport_now(); // to new area of the Abyss

            // It's effectively a new level, make a checkpoint save so eventual
            // crashes lose less of the player's progress (and fresh new bad
            // mutations).
            if (!crawl_state.disables[DIS_SAVE_CHECKPOINTS])
                save_game(false);
        }
        else if (you.form == transformation::wisp && !you.stasis())
            uncontrolled_blink();
    }

    abyss_maybe_spawn_xp_exit();

    actor_apply_cloud(&you);

    if (env.level_state & LSTATE_SLIMY_WALL)
        slime_wall_damage(&you, you.time_taken);

    // Icy shield and armour melt over lava.
    if (grd(you.pos()) == DNGN_LAVA)
        maybe_melt_player_enchantments(BEAM_FIRE, you.time_taken);

    // Handle starvation before subtracting hunger for this turn (including
    // hunger from the berserk duration) and before monsters react, so you
    // always get a turn (though it may be a delay or macro!) between getting
    // the Fainting light and actually fainting.
    handle_starvation();

    _decrement_durations();
    if (you.species == SP_GHOUL)
        mesmerise_hungry_players(you.time_taken);

    // Translocations and possibly other duration decrements can
    // escape a player from beholders and fearmongers. These should
    // update after.
    you.update_beholders();
    you.update_fearmongers();

    you.handle_constriction();

    // increment constriction durations
    you.accum_has_constricted();

    int food_tick = BASELINE_DELAY;
    if (have_passive(passive_t::slow_metabolism))
        food_tick *= 2;
    if (you.form == transformation::statue)
        food_tick = (food_tick * 3) / 2;
    const int food_use = div_rand_round(player_hunger_rate() * you.time_taken, food_tick);
    if (food_use > 0 && you.hunger > 0)
        make_hungry(food_use, true);

    regenerate_hp_and_mp(you.time_taken);

    dec_disease_player(you.time_taken);
    if (you.duration[DUR_POISONING])
        handle_player_poison(you.time_taken);

    // Reveal adjacent mimics.
    for (adjacent_iterator ai(you.pos(), false); ai; ++ai)
        discover_mimic(*ai);

    // Player stealth check.
    seen_monsters_react(stealth);

    // XOM now ticks from here, to increase his reaction time to tension.
    if (you_worship(GOD_XOM)
            || (you.char_class == JOB_CHAOS_KNIGHT && !you_worship(GOD_ZIN)))
        xom_tick();
    if (you_worship(GOD_QAZLAL))
        qazlal_storm_clouds();

    if (you.props[EMERGENCY_FLIGHT_KEY].get_bool())
        _handle_emergency_flight();
}

void extract_manticore_spikes(const char* endmsg)
{
    if (_decrement_a_duration(DUR_BARBS, you.time_taken, endmsg))
    {
        // Note: When this is called in _move player(), ATTR_BARBS_POW
        // has already been used to calculated damage for the player.
        // Otherwise, this prevents the damage.

        you.attribute[ATTR_BARBS_POW] = 0;

        you.props.erase(BARBS_MOVE_KEY);
    }
}
