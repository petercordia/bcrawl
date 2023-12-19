/**
 * @file
 * @brief Functions for using some of the wackier inventory items.
**/

#include "AppHdr.h"

#include "evoke.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "act-iter.h"
#include "areas.h"
#include "artefact.h"
#include "art-enum.h"
#include "branch.h"
#include "chardump.h"
#include "cloud.h"
#include "coordit.h"
#include "delay.h"
#include "directn.h"
#include "dungeon.h"
#include "english.h"
#include "env.h"
#include "exercise.h"
#include "fight.h"
#include "food.h"
#include "god-abil.h"
#include "god-conduct.h"
#include "invent.h"
#include "item-prop.h"
#include "item-status-flag-type.h"
#include "items.h"
#include "libutil.h"
#include "losglobal.h"
#include "message.h"
#include "mgen-data.h"
#include "misc.h"
#include "mon-behv.h"
#include "mon-clone.h"
#include "mon-pick.h"
#include "mon-place.h"
#include "mutant-beast.h"
#include "place.h"
#include "player.h"
#include "player-stats.h"
#include "religion.h"
#include "shout.h"
#include "skills.h"
#include "spl-book.h"
#include "spl-cast.h"
#include "spl-clouds.h"
#include "spl-util.h"
#include "state.h"
#include "stringutil.h"
#include "target.h"
#include "terrain.h"
#include "throw.h"
#ifdef USE_TILE
 #include "tilepick.h"
#endif
#include "transform.h"
#include "traps.h"
#include "unicode.h"
#include "view.h"

static bool _reaching_weapon_attack(const item_def& wpn)
{
    if (you.confused())
    {
        canned_msg(MSG_TOO_CONFUSED);
        return false;
    }

    if (you.caught())
    {
        mprf("You cannot attack while %s.", held_status());
        return false;
    }

    bool targ_mid = false;
    dist beam;

    direction_chooser_args args;
    args.restricts = DIR_TARGET;
    args.mode = TARG_HOSTILE;
    args.range = 2;
    args.top_prompt = "Attack whom?";
    args.self = CONFIRM_CANCEL;
    targeter_reach hitfunc(&you, REACH_TWO);
    args.hitfunc = &hitfunc;

    direction(beam, args);

    if (!beam.isValid)
    {
        if (beam.isCancel)
            canned_msg(MSG_OK);
        return false;
    }

    if (beam.isMe())
    {
        canned_msg(MSG_UNTHINKING_ACT);
        return false;
    }

    const coord_def delta = beam.target - you.pos();
    const int x_distance  = abs(delta.x);
    const int y_distance  = abs(delta.y);
    monster* mons = monster_at(beam.target);
    // don't allow targeting of submerged monsters
    if (mons && mons->submerged())
        mons = nullptr;

    const int x_first_middle = you.pos().x + (delta.x)/2;
    const int y_first_middle = you.pos().y + (delta.y)/2;
    const int x_second_middle = beam.target.x - (delta.x)/2;
    const int y_second_middle = beam.target.y - (delta.y)/2;
    const coord_def first_middle(x_first_middle, y_first_middle);
    const coord_def second_middle(x_second_middle, y_second_middle);

    if (x_distance > 2 || y_distance > 2)
    {
        mpr("Your weapon cannot reach that far!");
        return false;
    }

    // Calculate attack delay now in case we have to apply it.
    const int attack_delay = you.attack_delay().roll();

    if (!feat_is_reachable_past(grd(first_middle))
        && !feat_is_reachable_past(grd(second_middle)))
    {
        canned_msg(MSG_SOMETHING_IN_WAY);
        return false;
    }

    // Failing to hit someone due to a friend blocking is infuriating,
    // shadow-boxing empty space is not (and would be abusable to wait
    // with no penalty).
    if (mons)
        you.apply_berserk_penalty = false;

    // Choose one of the two middle squares (which might be the same).
    const coord_def middle =
        !feat_is_reachable_past(grd(first_middle)) ? second_middle :
        !feat_is_reachable_past(grd(second_middle)) ? first_middle :
        random_choose(first_middle, second_middle);

    // Check for a monster in the way. If there is one, it blocks the reaching
    // attack 50% of the time, and the attack tries to hit it if it is hostile.

    // If we're attacking more than a space away...
    if (x_distance > 1 || y_distance > 1)
    {
        bool success = true;
        monster *midmons;
        if ((midmons = monster_at(middle))
            && !midmons->submerged())
        {
            // This chance should possibly depend on your skill with
            // the weapon.
            if (coinflip())
            {
                success = false;
                beam.target = middle;
                mons = midmons;
                targ_mid = true;
                if (mons->wont_attack())
                {
                    // Let's assume friendlies cooperate.
                    mpr("You could not reach far enough!");
                    you.time_taken = attack_delay;
                    player_attack_hunger(attack_delay);
                    return true;
                }
            }
        }
        if (success)
            mpr("You reach to attack!");
        else
        {
            mprf("%s is in the way.",
                 mons->observable() ? mons->name(DESC_THE).c_str()
                                    : "Something you can't see");
        }
    }

    if (mons == nullptr)
    {
        // Must return true, otherwise you get a free discovery
        // of invisible monsters.
        mpr("You attack empty space.");
        you.time_taken = attack_delay;
        player_attack_hunger(attack_delay);
        return true;
    }
    else if (!fight_melee(&you, mons))
    {
        if (targ_mid)
        {
            // turn_is_over may have been reset to false by fight_melee, but
            // a failed attempt to reach further should not be free; instead,
            // charge the same as a successful attempt.
            you.time_taken = attack_delay;
            player_attack_hunger(attack_delay);
            you.turn_is_over = true;
        }
        else
            return false;
    }

    return true;
}

static bool _evoke_horn_of_geryon()
{
    bool created = false;

    if (silenced(you.pos()))
    {
        mpr("You can't produce a sound!");
        return false;
    }

    mprf(MSGCH_SOUND, "You produce a hideous howling noise!");
    noisy(15, you.pos()); // same as hell effect noise
    did_god_conduct(DID_EVIL, 3);
    int num = 1;
    const int adjusted_power =
        player_adjust_evoc_power(you.skill(SK_EVOCATIONS, 10));
    if (adjusted_power + random2(90) > 130)
        ++num;
    if (adjusted_power + random2(90) > 180)
        ++num;
    if (adjusted_power + random2(90) > 230)
        ++num;
    for (int n = 0; n < num; ++n)
    {
        monster* mon;
        beh_type beh = BEH_HOSTILE;
        bool will_anger = player_will_anger_monster(MONS_HELL_BEAST);

        if (!will_anger && random2(adjusted_power) > 7)
            beh = BEH_FRIENDLY;
        mgen_data mg(MONS_HELL_BEAST, beh, you.pos(), MHITYOU, MG_FORCE_BEH);
        mg.set_summoned(&you, 3, SPELL_NO_SPELL);
        mg.set_prox(PROX_CLOSE_TO_PLAYER);
        mon = create_monster(mg);
        if (mon)
            created = true;
        if (mon && will_anger)
        {
            mprf("%s is enraged by your holy aura!",
                 mon->name(DESC_THE).c_str());
        }
    }
    if (!created)
        mpr("Nothing answers your call.");
    return true;
}

/**
 * Spray lightning in all directions. (Randomly: shock, lightning bolt, OoE.)
 *
 * @param range         The range of the beams. (As with all beams, eventually
 *                      capped at LOS.)
 * @param power         The power of the beams. (Affects damage.)
 */
static void _spray_lightning(int range, int power)
{
    const zap_type which_zap = random_choose(ZAP_SHOCK,
                                             ZAP_LIGHTNING_BOLT,
                                             ZAP_ORB_OF_ELECTRICITY);

    bolt beam;
    // range has no tracer, so randomness is ok
    beam.range = range;
    beam.source = you.pos();
    beam.target = you.pos();
    beam.target.x += random2(13) - 6;
    beam.target.y += random2(13) - 6;
    // Non-controlleable, so no player tracer.
    zapping(which_zap, power, beam);
}

/**
 * Evoke a lightning rod, creating an arc of lightning that can be sustained
 * by continuing to evoke.
 *
 * @return  Whether anything happened.
 */
static bool _lightning_rod()
{
    if (you.confused())
    {
        canned_msg(MSG_TOO_CONFUSED);
        return false;
    }

    int power = player_adjust_evoc_power(5 + you.skill(SK_EVOCATIONS, 3), 0);

    const spret ret = your_spells(SPELL_THUNDERBOLT, power, false);

    if (ret == spret::abort)
        return false;

    return true;
}

/**
 * Spray lightning in all directions around the player.
 *
 * Quantity, range & power increase with level.
 */
void black_drac_breath()
{
    const int num_shots = roll_dice(2, 1 + you.experience_level / 7);
    const int range = you.experience_level / 3 + 5; // 5--14
    const int power = 25 + you.experience_level; // 25-52
    for (int i = 0; i < num_shots; ++i)
        _spray_lightning(range, power);
}

/**
 * Returns the MP cost of zapping a wand:
 *     3 if player has MP-powered wands and enough MP available,
 *     1-2 if player has MP-powered wands and only 1-2 MP left,
 *     0 otherwise.
 */
int wand_mp_cost()
{
    // Update mutation-data.h when updating this value.
    return min(you.magic_points, you.get_mutation_level(MUT_MP_WANDS) * 3);
}

int wand_power()
{
    const int mp_cost = wand_mp_cost();
    return (15 + you.skill(SK_EVOCATIONS, 7) / 2) * (mp_cost + 6) / 6;
}

void zap_wand(int slot)
{
    if (inv_count() < 1)
    {
        canned_msg(MSG_NOTHING_CARRIED);
        return;
    }

    if (you.confused())
    {
        canned_msg(MSG_TOO_CONFUSED);
        return;
    }

    if (you.berserk() && !you.wearing(EQ_AMULET, AMU_RAGE))
    {
        canned_msg(MSG_TOO_BERSERK);
        return;
    }

    if (you.get_mutation_level(MUT_NO_ARTIFICE))
    {
        mpr("You cannot evoke magical items.");
        return;
    }

    int item_slot;
    if (slot != -1)
        item_slot = slot;
    else
    {
        item_slot = prompt_invent_item("Zap which item?",
                                       MT_INVLIST,
                                       OBJ_WANDS,
                                       OPER_ZAP);
    }

    if (prompt_failed(item_slot))
        return;

    item_def& wand = you.inv[item_slot];
    if (wand.base_type != OBJ_WANDS)
    {
        mpr("You can't zap that!");
        return;
    }
    if (item_type_removed(wand.base_type, wand.sub_type))
    {
        mpr("Sorry, this wand was removed!");
        return;
    }
    // If you happen to be wielding the wand, its display might change.
    if (you.equip[EQ_WEAPON] == item_slot)
        you.wield_change = true;

    const int mp_cost = wand_mp_cost();
    const int power = wand_power();

    const spell_type spell =
        spell_in_wand(static_cast<wand_type>(wand.sub_type));

    spret ret = your_spells(spell, power, false, &wand);

    if (ret == spret::abort)
        return;
    else if (ret == spret::fail)
    {
        canned_msg(MSG_NOTHING_HAPPENS);
        you.turn_is_over = true;
        return;
    }

    // Spend MP.
    if (mp_cost)
        dec_mp(mp_cost, false);

    // Take off a charge.
    wand.charges--;

    if (wand.charges == 0)
    {
        ASSERT(in_inventory(wand));

        mpr("The now-empty wand crumbles to dust.");
        dec_inv_item_quantity(wand.link, 1);
    }

    practise_evoking(1);
    count_action(CACT_EVOKE, EVOC_WAND);
    alert_nearby_monsters();

    you.turn_is_over = true;
}

void archaeologist_read_tome(item_def& tome)
{
    tome.base_type = OBJ_SCROLLS;
    tome.sub_type = SCR_ACQUIREMENT;
    item_colour(tome);
    item_set_appearance(tome);
    
    set_ident_flags(tome, ISFLAG_IDENT_MASK);
    set_ident_type(tome, true);
    
    mpr("You're now able to understand the dusty tome!");
    mpr("Part of it contains a powerful arcane formula!");
    mpr("Most of the book is about a legendary artefact.");
    switch(random2(11))
    {
    case 0:
        mpr("It claims this artefact was used as a power source by an ancient civilization, until it was buried after some disaster. The book seems to assume the reader is already familiar with that disaster, and doesn't describe it clearly.");
        break;
    case 1:
        mpr("Supposedly this artefact could grant its holder eternal life, but with some unclear side effects. The book describes a war being fought over it until it was stolen by a powerful wizard.");
        break;
    case 2:
        mpr("Supposedly this artefact emitted energy that could sustain life without food or drink, and would cause animals to gradually become larger and stronger.");
        break;
    case 3:
        mpr("It claims this artefact was actually a hole in reality leading to a dimension of primordial energy, and that it could not safely be destroyed.");
        break;
    case 4:
        mpr("It claims this artefact was created by a master of translocation magic, which supposedly proves the superiority of translocations to the other schools of magic.");
        break;
    case 5:
        mpr("It claims this artefact was created by a master of summoning magic, which supposedly proves the superiority of summonings to the other schools of magic.");
        break;
    case 6:
        mpr("It claims this artefact was created by the god Lugonu, and goes on to praise Lugonu at length.");
        break;
    case 7:
        mpr("It claims this artefact was created by the god Ru, and goes on to praise Ru at length.");
        break;
    case 8:
        mpr("It claims this artefact was created by the god Ashenzari, and goes on to praise Ashenzari at length.");
        break;
    case 9:
        mpr("It describes this artefact being used by an ancient king who exposed subjects to its energy, which strengthened them but warped their minds. That king then had them killed en masse, which released some of that energy to grant strength with fewer side effects. The book then describes this practice leading to a revolt and civil war.");
        break;
    case 10:
        mpr("It claims this artefact weakened the barrier between dimensions, allowing demons to enter this world and also attracting them somehow.");
        break;
    default: break;
    }
    
    you.props[ARCHAEOLOGIST_TRIGGER_TOME_ON_PICKUP] = false;
}

string manual_skill_names(bool short_text)
{
    skill_set skills;
    for (skill_type sk = SK_FIRST_SKILL; sk < NUM_SKILLS; sk++)
        if (you.skill_manual_points[sk])
            skills.insert(sk);

    if (short_text && skills.size() > 1)
    {
        char buf[40];
        sprintf(buf, "%lu skills", (unsigned long) skills.size());
        return string(buf);
    }
    else
        return skill_names(skills);
}

static bool _box_of_beasts(item_def &box)
{
    mpr("You open the lid...");

    // two rolls to reduce std deviation - +-6 so can get < max even at 27 sk
    int rnd_factor = random2(7);
    rnd_factor -= random2(7);
    const int hd_min = min(27,
            player_adjust_evoc_power(you.skill(SK_EVOCATIONS) + rnd_factor));
    const int tier = mutant_beast_tier(hd_min);
    ASSERT(tier < NUM_BEAST_TIERS);

    mgen_data mg(MONS_MUTANT_BEAST, BEH_FRIENDLY, you.pos(), MHITYOU,
                 MG_AUTOFOE);
    mg.set_summoned(&you, 3 + random2(3), 0);

    mg.hd = beast_tiers[tier];
    dprf("hd %d (min %d, tier %d)", mg.hd, hd_min, tier);
    const monster* mons = create_monster(mg);

    if (!mons)
    {
        // Failed to create monster for some reason
        mpr("...but nothing happens.");
        return false;
    }

    mprf("...and %s %s out!",
         mons->name(DESC_A).c_str(), mons->airborne() ? "flies" : "leaps");
    did_god_conduct(DID_CHAOS, random_range(5,10));

    // After unboxing a beast, chance to break.
    if (one_chance_in(3))
    {
        mpr("The now-empty box falls apart.");
        ASSERT(in_inventory(box));
        dec_inv_item_quantity(box.link, 1);
    }

    return true;
}

static bool _sack_of_spiders(item_def &sack)
{
    monster_type mon = random_choose_weighted(
            10, MONS_REDBACK,
            5, MONS_WOLF_SPIDER,
            4, MONS_JUMPING_SPIDER,
            3, MONS_TARANTELLA,
            3, MONS_ORB_SPIDER);
    
    int monster_value = 70;
    switch(mon)
    {
    case MONS_REDBACK: monster_value = 70; break;
    case MONS_WOLF_SPIDER: monster_value = 140; break;
    case MONS_JUMPING_SPIDER: monster_value = 140; break;
    case MONS_TARANTELLA: monster_value = 140; break;
    case MONS_ORB_SPIDER: monster_value = 140; break;
    default: break;
    }
    
    int power = 70 + you.skill(SK_EVOCATIONS, 10);
    int x = power*power;
    int y = 140*monster_value;
    int count = random2(div_rand_round(x, y) + 1) + random2(div_rand_round(x, y) + 1);

    int summon_count = 0;
    for (int n = 0; n < count; n++)
    {
        mgen_data mg(mon, BEH_FRIENDLY, you.pos(), MHITYOU, MG_AUTOFOE);
        mg.set_summoned(&you, 3 + random2(4), 0);
        if (create_monster(mg))
            summon_count++;
    }

    string message_end = "";
    if (summon_count)
    {
        if (summon_count > 1)
            message_end = "and spiders crawl out!";
        else
            message_end = "and a spider crawls out!";

        // for backwards compatibility
        if (sack.quantity > 1)
            dec_inv_item_quantity(sack.link, sack.quantity - 1);
    }
    else
        message_end = "but nothing happens.";

    mprf("You reach into the bag, %s", message_end.c_str());

    return (summon_count > 0);
}

static bool _make_zig(item_def &zig)
{
    if (feat_is_critical(grd(you.pos())))
    {
        mpr("You can't place a gateway to a ziggurat here.");
        return false;
    }
    for (int lev = 1; lev <= brdepth[BRANCH_ZIGGURAT]; lev++)
    {
        if (is_level_on_stack(level_id(BRANCH_ZIGGURAT, lev))
            || you.where_are_you == BRANCH_ZIGGURAT)
        {
            mpr("Finish your current ziggurat first!");
            return false;
        }
    }

    ASSERT(in_inventory(zig));
    dec_inv_item_quantity(zig.link, 1);
    dungeon_terrain_changed(you.pos(), DNGN_ENTER_ZIGGURAT);
    mpr("You set the figurine down, and a mystic portal to a ziggurat forms.");
    return true;
}

static vector<coord_def> _get_jitter_path(coord_def source, coord_def target,
                                          bool jitter_start,
                                          bolt &beam1, bolt &beam2)
{
    const int NUM_TRIES = 10;
    const int RANGE = 8;

    bolt trace_beam;
    trace_beam.source = source;
    trace_beam.target = target;
    trace_beam.aimed_at_spot = false;
    trace_beam.is_tracer = true;
    trace_beam.range = RANGE;
    trace_beam.fire();

    coord_def aim_dir = (source - target).sgn();

    if (trace_beam.path_taken.back() != source)
        target = trace_beam.path_taken.back();

    if (jitter_start)
    {
        for (int n = 0; n < NUM_TRIES; ++n)
        {
            coord_def jitter_rnd;
            jitter_rnd.x = random_range(-2, 2);
            jitter_rnd.y = random_range(-2, 2);
            coord_def jitter = clamp_in_bounds(target + jitter_rnd);
            if (jitter == target || jitter == source || cell_is_solid(jitter))
                continue;

            trace_beam.target = jitter;
            trace_beam.fire();

            coord_def delta = source - trace_beam.path_taken.back();
            //Don't try to aim at targets in the opposite direction of main aim
            if ((abs(aim_dir.x - delta.sgn().x) + abs(aim_dir.y - delta.sgn().y) >= 2)
                 && !delta.origin())
            {
                continue;
            }

            target = trace_beam.path_taken.back();
            break;
        }
    }

    vector<coord_def> path = trace_beam.path_taken;
    unsigned int mid_i = (path.size() / 2);
    coord_def mid = path[mid_i];

    for (int n = 0; n < NUM_TRIES; ++n)
    {
        coord_def jitter_rnd;
        jitter_rnd.x = random_range(-3, 3);
        jitter_rnd.y = random_range(-3, 3);
        coord_def jitter = clamp_in_bounds(mid + jitter_rnd);
        if (jitter == mid || jitter.distance_from(mid) < 2 || jitter == source
            || cell_is_solid(jitter)
            || !cell_see_cell(source, jitter, LOS_NO_TRANS)
            || !cell_see_cell(target, jitter, LOS_NO_TRANS))
        {
            continue;
        }

        trace_beam.aimed_at_feet = false;
        trace_beam.source = jitter;
        trace_beam.target = target;
        trace_beam.fire();

        coord_def delta1 = source - jitter;
        coord_def delta2 = source - trace_beam.path_taken.back();

        //Don't try to aim at targets in the opposite direction of main aim
        if (abs(aim_dir.x - delta1.sgn().x) + abs(aim_dir.y - delta1.sgn().y) >= 2
            || abs(aim_dir.x - delta2.sgn().x) + abs(aim_dir.y - delta2.sgn().y) >= 2)
        {
            continue;
        }

        // Don't make l-turns
        coord_def delta = jitter-target;
        if (!delta.x || !delta.y)
            continue;

        if (find(begin(path), end(path), jitter) != end(path))
            continue;

        mid = jitter;
        break;
    }

    beam1.source = source;
    beam1.target = mid;
    beam1.range = RANGE;
    beam1.aimed_at_spot = true;
    beam1.is_tracer = true;
    beam1.fire();
    beam1.is_tracer = false;

    beam2.source = mid;
    beam2.target = target;
    beam2.range = max(int(RANGE - beam1.path_taken.size()), mid.distance_from(target));
    beam2.is_tracer = true;
    beam2.fire();
    beam2.is_tracer = false;

    vector<coord_def> newpath;
    newpath.insert(newpath.end(), beam1.path_taken.begin(), beam1.path_taken.end());
    newpath.insert(newpath.end(), beam2.path_taken.begin(), beam2.path_taken.end());

    return newpath;
}

static bool _check_path_overlap(const vector<coord_def> &path1,
                                const vector<coord_def> &path2, int match_len)
{
    int max_len = min(path1.size(), path2.size());
    match_len = min(match_len, max_len-1);

    // Check for overlap with previous path
    int matchs = 0;
    for (int i = 0; i < max_len; ++i)
    {
        if (path1[i] == path2[i])
            ++matchs;
        else
            matchs = 0;

        if (matchs >= match_len)
            return true;
    }

    return false;
}

static bool _fill_flame_trails(coord_def source, coord_def target,
                               vector<bolt> &beams, int num)
{
    const int NUM_TRIES = 10;
    vector<vector<coord_def> > paths;
    for (int n = 0; n < num; ++n)
    {
        int tries = 0;
        vector<coord_def> path;
        bolt beam1, beam2;
        while (++tries <= NUM_TRIES && path.empty())
        {
            path = _get_jitter_path(source, target, !paths.empty(), beam1, beam2);
            for (const vector<coord_def> &oldpath : paths)
            {
                if (_check_path_overlap(path, oldpath, 3))
                {
                    path.clear();
                    beam1 = bolt();
                    beam2 = bolt();
                    break;
                }
            }
        }

        if (!path.empty())
        {
            paths.push_back(path);
            beams.push_back(beam1);
            beams.push_back(beam2);
        }
    }

    return !paths.empty();
}

static bool _lamp_of_fire()
{
    bolt base_beam;
    dist target;
    direction_chooser_args args;
    args.restricts = DIR_TARGET;
    args.mode = TARG_HOSTILE;
    args.top_prompt = "Aim the lamp in which direction?";
    args.self = CONFIRM_CANCEL;
    if (spell_direction(target, base_beam, &args))
    {
        if (you.confused())
            target.confusion_fuzz();

        mpr("The flames dance!");

        vector<bolt> beams;
        int randomized_power = you.skill(SK_EVOCATIONS, 10) + random2(70) - 50;
        int num_trails = 1 + max(0, randomized_power/30);

        _fill_flame_trails(you.pos(), target.target, beams, num_trails);

        const int pow =
            player_adjust_evoc_power(8 + you.skill_rdiv(SK_EVOCATIONS, 14, 4));
        for (bolt &beam : beams)
        {
            if (beam.source == beam.target)
                continue;

            beam.flavour    = BEAM_FIRE;
            beam.colour     = RED;
            beam.source_id  = MID_PLAYER;
            beam.thrower    = KILL_YOU;
            beam.pierce     = true;
            beam.name       = "trail of fire";
            beam.hit        = 10 + (pow/8);
            beam.damage     = dice_def(2, 5 + pow/4);
            beam.ench_power = 3 + (pow/5);
            beam.loudness   = 5;
            beam.fire();
        }
        return true;
    }

    return false;
}

struct dist_sorter
{
    coord_def pos;
    bool operator()(const actor* a, const actor* b)
    {
        return a->pos().distance_from(pos) > b->pos().distance_from(pos);
    }
};

static int _gale_push_dist(const actor* agent, const actor* victim, int pow)
{
    int dist = 1 + random2(pow / 20);

    if (victim->airborne())
        dist++;

    if (victim->body_size(PSIZE_BODY) < SIZE_MEDIUM)
        dist++;
    else if (victim->body_size(PSIZE_BODY) > SIZE_BIG)
        dist /= 2;
    else if (victim->body_size(PSIZE_BODY) > SIZE_MEDIUM)
        dist -= 1;

    int range = victim->pos().distance_from(agent->pos());
    if (range > 5)
        dist -= 2;
    else if (range > 2)
        dist--;

    if (dist < 0)
        return 0;
    else
        return dist;
}

static double _angle_between(coord_def origin, coord_def p1, coord_def p2)
{
    double ang0 = atan2(p1.x - origin.x, p1.y - origin.y);
    double ang  = atan2(p2.x - origin.x, p2.y - origin.y);
    return min(fabs(ang - ang0), fabs(ang - ang0 + 2 * PI));
}

void wind_blast(actor* agent, int pow, coord_def target, bool card)
{
    vector<actor *> act_list;

    int radius = min(5, 4 + div_rand_round(pow, 60));

    for (actor_near_iterator ai(agent->pos(), LOS_SOLID); ai; ++ai)
    {
        if (ai->is_stationary()
            || ai->pos().distance_from(agent->pos()) > radius
            || ai->pos() == agent->pos() // so it's never aimed_at_feet
            || !target.origin()
               && _angle_between(agent->pos(), target, ai->pos()) > PI/4.0)
        {
            continue;
        }

        act_list.push_back(*ai);
    }

    dist_sorter sorter = {agent->pos()};
    sort(act_list.begin(), act_list.end(), sorter);

    bolt wind_beam;
    wind_beam.hit             = AUTOMATIC_HIT;
    wind_beam.pierce          = true;
    wind_beam.affects_nothing = true;
    wind_beam.source          = agent->pos();
    wind_beam.range           = LOS_RADIUS;
    wind_beam.is_tracer       = true;

    map<actor *, coord_def> collisions;

    bool player_affected = false;
    counted_monster_list affected_monsters;

    for (actor *act : act_list)
    {
        wind_beam.target = act->pos();
        wind_beam.fire();

        int push = _gale_push_dist(agent, act, pow);
        bool pushed = false;

        for (unsigned int j = 0; j < wind_beam.path_taken.size() - 1 && push;
             ++j)
        {
            if (wind_beam.path_taken[j] == act->pos())
            {
                coord_def newpos = wind_beam.path_taken[j+1];
                if (!actor_at(newpos) && !cell_is_solid(newpos)
                    && act->can_pass_through(newpos)
                    && act->is_habitable(newpos))
                {
                    act->move_to_pos(newpos);
                    if (act->is_player())
                        stop_delay(true);
                    --push;
                    pushed = true;
                }
                else //Try to find an alternate route to push
                {
                    bool success = false;
                    for (adjacent_iterator di(newpos); di; ++di)
                    {
                        if (adjacent(*di, act->pos())
                            && di->distance_from(agent->pos())
                                == newpos.distance_from(agent->pos())
                            && !actor_at(*di) && !cell_is_solid(*di)
                            && act->can_pass_through(*di)
                            && act->is_habitable(*di))
                        {
                            act->move_to_pos(*di);
                            if (act->is_player())
                                stop_delay(true);

                            --push;
                            pushed = true;

                            // Adjust wind path for moved monster
                            wind_beam.target = *di;
                            wind_beam.fire();
                            success = true;
                            break;
                        }
                    }

                    // If no luck, they slam into something.
                    if (!success)
                        collisions.insert(make_pair(act, newpos));
                }
            }
        }

        if (pushed)
        {
            if (act->is_monster())
            {
                act->as_monster()->speed_increment -= random2(6) + 4;
                if (you.can_see(*act))
                    affected_monsters.add(act->as_monster());
            }
            else
                player_affected = true;
        }
    }

    // Now move clouds
    vector<coord_def> cloud_list;
    for (distance_iterator di(agent->pos(), true, false, radius + 2); di; ++di)
    {
        if (cloud_at(*di)
            && cell_see_cell(agent->pos(), *di, LOS_SOLID)
            && (target.origin()
                || _angle_between(agent->pos(), target, *di) <= PI/4.0))
        {
            cloud_list.push_back(*di);
        }
    }

    for (int i = cloud_list.size() - 1; i >= 0; --i)
    {
        wind_beam.target = cloud_list[i];
        wind_beam.fire();

        int dist = cloud_list[i].distance_from(agent->pos());
        int push = (dist > 5 ? 2 : dist > 2 ? 3 : 4);

        if (dist == 0 && agent->is_player())
        {
            delete_cloud(agent->pos());
            break;
        }

        for (unsigned int j = 0;
             j < wind_beam.path_taken.size() - 1 && push;
             ++j)
        {
            if (wind_beam.path_taken[j] == cloud_list[i])
            {
                coord_def newpos = wind_beam.path_taken[j+1];
                if (!cell_is_solid(newpos)
                    && !cloud_at(newpos))
                {
                    swap_clouds(newpos, wind_beam.path_taken[j]);
                    --push;
                }
                else //Try to find an alternate route to push
                {
                    for (distance_iterator di(wind_beam.path_taken[j],
                         false, true, 1); di; ++di)
                    {
                        if (di->distance_from(agent->pos())
                                == newpos.distance_from(agent->pos())
                            && *di != agent->pos() // never aimed_at_feet
                            && !cell_is_solid(*di)
                            && !cloud_at(*di))
                        {
                            swap_clouds(*di, wind_beam.path_taken[j]);
                            --push;
                            wind_beam.target = *di;
                            wind_beam.fire();
                            j--;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (agent->is_player())
    {
        const string source = card ? "card" : "fan";

        if (pow > 120)
            mprf("A mighty gale blasts forth from the %s!", source.c_str());
        else
            mprf("A fierce wind blows from the %s.", source.c_str());
    }

    noisy(8, agent->pos());

    if (player_affected)
        mpr("You are blown backwards!");

    if (!affected_monsters.empty())
    {
        const string message =
            make_stringf("%s %s blown away by the wind.",
                         affected_monsters.describe().c_str(),
                         conjugate_verb("be", affected_monsters.count() > 1).c_str());
        if (strwidth(message) < get_number_of_cols() - 2)
            mpr(message);
        else
            mpr("The monsters around you are blown away!");
    }

    for (auto it : collisions)
        if (it.first->alive())
            it.first->collide(it.second, agent, pow);
}

static bool _phial_of_floods()
{
    dist target;
    bolt beam;

    const int base_pow = 10 + you.skill(SK_EVOCATIONS, 4); // placeholder?
    zappy(ZAP_PRIMAL_WAVE, base_pow, false, beam);
    beam.range = LOS_RADIUS;
    beam.aimed_at_spot = true;

    direction_chooser_args args;
    args.mode = TARG_HOSTILE;
    args.top_prompt = "Aim the phial where?";
    if (spell_direction(target, beam, &args)
        && player_tracer(ZAP_PRIMAL_WAVE, base_pow, beam))
    {
        if (you.confused())
        {
            target.confusion_fuzz();
            beam.set_target(target);
        }

        const int power = player_adjust_evoc_power(base_pow);
        // use real power to recalc hit/dam
        zappy(ZAP_PRIMAL_WAVE, power, false, beam);
        beam.hit = AUTOMATIC_HIT;
        beam.fire();

        vector<coord_def> elementals;
        // Flood the endpoint
        coord_def center = beam.path_taken.back();
        const int rnd_factor = random2(7);
        int num = player_adjust_evoc_power(
                      5 + you.skill_rdiv(SK_EVOCATIONS, 3, 5) + rnd_factor);
        int dur = player_adjust_evoc_power(
                      40 + you.skill_rdiv(SK_EVOCATIONS, 8, 3));
        for (distance_iterator di(center, true, false, 2); di && num > 0; ++di)
        {
            const dungeon_feature_type feat = grd(*di);
            if ((feat == DNGN_FLOOR || feat == DNGN_SHALLOW_WATER)
                && cell_see_cell(center, *di, LOS_NO_TRANS))
            {
                num--;
                temp_change_terrain(*di, DNGN_SHALLOW_WATER,
                                    random_range(dur*2, dur*3) - (di.radius()*20),
                                    TERRAIN_CHANGE_FLOOD);
                elementals.push_back(*di);
            }
        }
        
        int evo_skill = you.skill(SK_EVOCATIONS, 10);
        int randomized_power = evo_skill + random2(70) - 50;
        int num_elementals = 1 + max(0, randomized_power/60);

        bool created = false;
        num = min(num_elementals,
                  min((int)elementals.size(), (int)elementals.size() / 5 + 1));
        beh_type attitude = BEH_FRIENDLY;
        if (player_will_anger_monster(MONS_WATER_ELEMENTAL))
            attitude = BEH_HOSTILE;
        for (int n = 0; n < num; ++n)
        {
            mgen_data mg (MONS_WATER_ELEMENTAL, attitude, elementals[n], 0,
                          MG_FORCE_BEH | MG_FORCE_PLACE);
            mg.set_summoned(&you, 3, SPELL_NO_SPELL);
            mg.hd = 6 + div_rand_round(evo_skill * 3, 70);
            if (create_monster(mg))
                created = true;
        }
        if (created)
            mpr("The water rises up and takes form.");

        return true;
    }

    return false;
}

static int _mirror_break_chance(int target_hd)
{
    int xl = you.experience_level;
    int scaled_hd = target_hd + (target_hd - 1)/2;
    int break_chance = (scaled_hd * scaled_hd * 100) / (xl * xl);
    return min(100, break_chance);
}

static vector<string> _desc_phantom_mirror_break(const monster_info& mi)
{
    int chance = _mirror_break_chance(mi.hd);
    vector<string> descs;
    descs.push_back(make_stringf("chance to break mirror: %d%%", chance));
    return descs;
}

static spret _phantom_mirror()
{
    bolt beam;
    monster* victim = nullptr;
    dist spd;
    targeter_smite tgt(&you, LOS_RADIUS, 0, 0);

    desc_filter additional_desc = bind(_desc_phantom_mirror_break, placeholders::_1);
    direction_chooser_args args;
    args.restricts = DIR_TARGET;
    args.needs_path = false;
    args.self = CONFIRM_CANCEL;
    args.top_prompt = "Aiming: <white>Phantom Mirror</white>";
    args.get_desc_func = additional_desc;
    args.hitfunc = &tgt;
    if (!spell_direction(spd, beam, &args))
        return spret::abort;
    victim = monster_at(beam.target);
    if (!victim || !you.can_see(*victim))
    {
        if (beam.target == you.pos())
            mpr("You can't use the mirror on yourself.");
        else
            mpr("You can't see anything there to clone.");
        return spret::abort;
    }

    if (victim->is_summoned() || !actor_is_illusion_cloneable(victim)
            || mons_genus(victim->type) == MONS_VAMPIRE)
    {
        mpr("The mirror can't reflect that.");
        return spret::abort;
    }

    if (player_angers_monster(victim, false))
        return spret::abort;

    monster* mon = clone_mons(victim, true, nullptr, ATT_FRIENDLY);
    if (!mon)
    {
        canned_msg(MSG_NOTHING_HAPPENS);
        return spret::fail;
    }
    const int power = player_adjust_evoc_power(5 + you.skill(SK_EVOCATIONS, 3));
    int dur = min(6, max(1,
                         player_adjust_evoc_power(
                             you.skill(SK_EVOCATIONS, 1) / 4 + 1)
                         * (100 - victim->check_res_magic(power)) / 100));

    mon->mark_summoned(dur, true, SPELL_PHANTOM_MIRROR);

    mon->summoner = MID_PLAYER;
    mons_add_blame(mon, "mirrored by the player character");
    mon->add_ench(ENCH_PHANTOM_MIRROR);
    mon->add_ench(mon_enchant(ENCH_DRAINED,
                              div_rand_round(mon->get_experience_level(), 3),
                              &you, INFINITE_DURATION));

    // items can't copy divine enchantments
    mon->del_ench(ENCH_SOUL_RIPE);
    mon->del_ench(ENCH_PAIN_BOND);
    mon->del_ench(ENCH_MIRROR_DAMAGE);
    mon->del_ench(ENCH_GOLD_LUST);

    int break_chance = _mirror_break_chance(victim->get_hit_dice());
    bool mirror_break = x_chance_in_y(break_chance, 100);

    mon->behaviour = BEH_SEEK;
    set_nearest_monster_foe(mon);

    mprf("You reflect %s with the mirror%s!",
         victim->name(DESC_THE).c_str(), (mirror_break ? ", and the mirror shatters" : ""));

    return mirror_break ? spret::success : spret::fail;
}

bool evoke_check(int slot, bool quiet)
{
    const bool reaching = slot != -1 && slot == you.equip[EQ_WEAPON]
                          && !you.melded[EQ_WEAPON]
                          && weapon_reach(*you.weapon()) > REACH_NONE;

    if (you.berserk() && !reaching)
    {
        if (!quiet)
            canned_msg(MSG_TOO_BERSERK);
        return false;
    }
    return true;
}

bool evoke_item(int slot, bool check_range)
{
    if (!evoke_check(slot))
        return false;

    if (slot == -1)
    {
        slot = prompt_invent_item("Evoke which item? (* to show all)",
                                   MT_INVLIST,
                                   OSEL_EVOKABLE, OPER_EVOKE);

        if (prompt_failed(slot))
            return false;
    }
    else if (!check_warning_inscriptions(you.inv[slot], OPER_EVOKE))
        return false;

    ASSERT(slot >= 0);

#ifdef ASSERTS // Used only by an assert
    const bool wielded = (you.equip[EQ_WEAPON] == slot);
#endif /* DEBUG */

    item_def& item = you.inv[slot];
    // Also handles messages.
    if (!item_is_evokable(item, true, false, true))
        return false;

    bool did_work   = false;  // "Nothing happens" message
    bool unevokable = false;

    const unrandart_entry *entry = is_unrandom_artefact(item)
        ? get_unrand_entry(item.unrand_idx) : nullptr;

    if (entry && entry->evoke_func)
    {
        ASSERT(item_is_equipped(item));

        if (you.confused())
        {
            canned_msg(MSG_TOO_CONFUSED);
            return false;
        }

        bool qret = entry->evoke_func(&item, &did_work, &unevokable);

        if (!unevokable)
            count_action(CACT_EVOKE, item.unrand_idx);

        // what even _is_ this return value?
        if (qret)
            return did_work;
    }
    else switch (item.base_type)
    {
    case OBJ_WANDS:
        zap_wand(slot);
        return true;

    case OBJ_WEAPONS:
        ASSERT(wielded);

        if (weapon_reach(item) > REACH_NONE)
        {
            if (_reaching_weapon_attack(item))
                did_work = true;
            else
                return false;
        }
        else
            unevokable = true;
        break;

    case OBJ_STAVES:
        ASSERT(wielded);
        if (item.sub_type != STAFF_POWER)
        {
            unevokable = true;
            break;
        }

        if (you.confused())
        {
            canned_msg(MSG_TOO_CONFUSED);
            return false;
        }

        if (apply_starvation_penalties())
        {
            canned_msg(MSG_TOO_HUNGRY);
            return false;
        }
        else if (you.magic_points >= you.max_magic_points)
        {
            canned_msg(MSG_FULL_MAGIC);
            return false;
        }
        else if (x_chance_in_y(apply_enhancement(
                                   you.skill(SK_EVOCATIONS, 100) + 1100,
                                   you.spec_evoke()),
                               4000))
        {
            mpr("You channel some magical energy.");
            inc_mp(1 + random2(3));
            make_hungry(50, false, true);
            did_work = true;
            practise_evoking(1);
            count_action(CACT_EVOKE, STAFF_POWER, OBJ_STAVES);

            did_god_conduct(DID_CHANNEL, 1, true);
        }
        break;

    case OBJ_MISCELLANY:
        did_work = true; // easier to do it this way for misc items

        if ((you.get_mutation_level(MUT_NO_ARTIFICE))
            && item.sub_type != MISC_ZIGGURAT)
        {
            mpr("You cannot evoke magical items.");
            return false;
        }

        switch (item.sub_type)
        {
#if TAG_MAJOR_VERSION == 34
        case MISC_BOTTLED_EFREET:
            canned_msg(MSG_NOTHING_HAPPENS);
            return false;
#endif

        case MISC_FAN_OF_GALES:
        {
            if (!evoker_charges(item.sub_type))
            {
                mpr("That is presently inert.");
                return false;
            }

            wind_blast(&you,
                       player_adjust_evoc_power(you.skill(SK_EVOCATIONS, 15)),
                       coord_def());
            expend_xp_evoker(item.sub_type);
            practise_evoking(3);
            break;
        }

        case MISC_LAMP_OF_FIRE:
            if (!evoker_charges(item.sub_type))
            {
                mpr("That is presently inert.");
                return false;
            }
            if (_lamp_of_fire())
            {
                expend_xp_evoker(item.sub_type);
                practise_evoking(3);
            }
            else
                return false;

            break;

#if TAG_MAJOR_VERSION == 34
        case MISC_STONE_OF_TREMORS:
            canned_msg(MSG_NOTHING_HAPPENS);
            return false;
#endif

        case MISC_PHIAL_OF_FLOODS:
            if (!evoker_charges(item.sub_type))
            {
                mpr("That is presently inert.");
                return false;
            }
            if (_phial_of_floods())
            {
                expend_xp_evoker(item.sub_type);
                practise_evoking(3);
            }
            else
                return false;
            break;

        case MISC_HORN_OF_GERYON:
            if (!evoker_charges(item.sub_type))
            {
                mpr("That is presently inert.");
                return false;
            }
            if (_evoke_horn_of_geryon())
            {
                expend_xp_evoker(item.sub_type);
                practise_evoking(3);
            }
            else
                return false;
            break;

        case MISC_BOX_OF_BEASTS:
            if (_box_of_beasts(item))
                practise_evoking(1);
            break;

        case MISC_SACK_OF_SPIDERS:
            if (!evoker_charges(item.sub_type))
            {
                mpr("That is presently inert.");
                return false;
            }
            if (_sack_of_spiders(item))
            {
                expend_xp_evoker(item.sub_type);
                practise_evoking(3);
            }
            break;

        case MISC_LIGHTNING_ROD:
            if (!evoker_charges(item.sub_type))
            {
                mpr("That is presently inert.");
                return false;
            }
            if (_lightning_rod())
            {
                practise_evoking(1);
                expend_xp_evoker(item.sub_type);
                if (!evoker_charges(item.sub_type))
                    mpr("The lightning rod overheats!");
            }
            else
                return false;
            break;

        case MISC_QUAD_DAMAGE:
            mpr("QUAD DAMAGE!");
            you.duration[DUR_QUAD_DAMAGE] = 30 * BASELINE_DELAY;
            ASSERT(in_inventory(item));
            dec_inv_item_quantity(item.link, 1);
            invalidate_agrid(true);
            break;

        case MISC_PHANTOM_MIRROR:
            switch (_phantom_mirror())
            {
                default:
                case spret::abort:
                    return false;

                case spret::success:
                    ASSERT(in_inventory(item));
                    dec_inv_item_quantity(item.link, 1);
                    // deliberate fall-through
                case spret::fail:
                    practise_evoking(1);
                    break;
            }
            break;

        case MISC_ZIGGURAT:
            // Don't set did_work to false, _make_zig handles the message.
            unevokable = !_make_zig(item);
            break;

        default:
            did_work = false;
            unevokable = true;
            break;
        }
        if (did_work && !unevokable)
            count_action(CACT_EVOKE, item.sub_type, OBJ_MISCELLANY);
        break;

    default:
        unevokable = true;
        break;
    }

    if (!did_work)
        canned_msg(MSG_NOTHING_HAPPENS);

    if (!unevokable)
        you.turn_is_over = true;
    else
        crawl_state.zero_turns_taken();

    return did_work;
}
