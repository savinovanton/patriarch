#include "pat/data/army_schemas.hpp"

#include <limits>
#include <utility>
namespace pat::data {
namespace {
Type i32() {
    return Type::integer(std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
}
ObjectSchema rules_schema() {
    ObjectSchema s("army_rules");
    s.intern_last().describe_kind(
        "Предлагаемые правила армий M1; единицы: _pm — промилле, _ppm — миллионные, _x100 — сотые, _kop — "
        "копейки. Коридоры и связи проверяет sim::army::validate; загрузка opt-in до E16-T20.");
    {
        ObjectSchema b;
        b.required("world", i32()).describe("Параметр limits.world; формула и единицы — army-model.md §9.");
        b.required("dynasty", i32())
            .describe("Параметр limits.dynasty; формула и единицы — army-model.md §9.");
        b.required("crown", i32()).describe("Параметр limits.crown; формула и единицы — army-model.md §9.");
        b.required("rebels", i32()).describe("Параметр limits.rebels; формула и единицы — army-model.md §9.");
        b.required("min_size", i32())
            .describe("Параметр limits.min_size; формула и единицы — army-model.md §9.");
        b.required("min_rebels", i32())
            .describe("Параметр limits.min_rebels; формула и единицы — army-model.md §9.");
        b.required("max_size", i32())
            .describe("Параметр limits.max_size; формула и единицы — army-model.md §9.");
        s.required("limits", Type::block(std::move(b)));
    }
    {
        ObjectSchema b;
        b.required("slow_share_pm", i32())
            .describe("Параметр movement.slow_share_pm; формула и единицы — army-model.md §9.");
        b.required("road_pm", i32())
            .describe("Параметр movement.road_pm; формула и единицы — army-model.md §9.");
        b.required("march_drain_x100", i32())
            .describe("Параметр movement.march_drain_x100; формула и единицы — army-model.md §9.");
        s.required("movement", Type::block(std::move(b)));
    }
    {
        ObjectSchema b;
        b.required("loss_loser", i32())
            .describe("Параметр battle.loss_loser; формула и единицы — army-model.md §9.");
        b.required("loss_winner", i32())
            .describe("Параметр battle.loss_winner; формула и единицы — army-model.md §9.");
        b.required("max_loss", i32())
            .describe("Параметр battle.max_loss; формула и единицы — army-model.md §9.");
        b.required("morale_loser", i32())
            .describe("Параметр battle.morale_loser; формула и единицы — army-model.md §9.");
        b.required("morale_winner", i32())
            .describe("Параметр battle.morale_winner; формула и единицы — army-model.md §9.");
        b.required("p_min", i32()).describe("Параметр battle.p_min; формула и единицы — army-model.md §9.");
        b.required("p_max", i32()).describe("Параметр battle.p_max; формула и единицы — army-model.md §9.");
        b.required("quality_base", i32())
            .describe("Параметр battle.quality_base; формула и единицы — army-model.md §9.");
        b.required("quality_factor", i32())
            .describe("Параметр battle.quality_factor; формула и единицы — army-model.md §9.");
        b.required("experience_base", i32())
            .describe("Параметр battle.experience_base; формула и единицы — army-model.md §9.");
        b.required("morale_base", i32())
            .describe("Параметр battle.morale_base; формула и единицы — army-model.md §9.");
        b.required("supply_base", i32())
            .describe("Параметр battle.supply_base; формула и единицы — army-model.md §9.");
        b.required("commander_gain", i32())
            .describe("Параметр battle.commander_gain; формула и единицы — army-model.md §9.");
        b.required("commander_pivot", i32())
            .describe("Параметр battle.commander_pivot; формула и единицы — army-model.md §9.");
        b.required("fort_bonus", i32())
            .describe("Параметр battle.fort_bonus; формула и единицы — army-model.md §9.");
        b.required("weather_pm", i32())
            .describe("Параметр battle.weather_pm; формула и единицы — army-model.md §9.");
        b.required("surprise_pm", i32())
            .describe("Параметр battle.surprise_pm; формула и единицы — army-model.md §9.");
        b.required("no_retreat_extra_loss_pm", i32())
            .describe("Параметр battle.no_retreat_extra_loss_pm; формула и единицы — army-model.md §9.");
        b.required("experience_winner", i32())
            .describe("Параметр battle.experience_winner; формула и единицы — army-model.md §9.");
        b.required("experience_loser", i32())
            .describe("Параметр battle.experience_loser; формула и единицы — army-model.md §9.");
        b.required("kill_loser_pm", i32())
            .describe("Параметр battle.kill_loser_pm; формула и единицы — army-model.md §9.");
        b.required("wound_loser_pm", i32())
            .describe("Параметр battle.wound_loser_pm; формула и единицы — army-model.md §9.");
        b.required("kill_winner_pm", i32())
            .describe("Параметр battle.kill_winner_pm; формула и единицы — army-model.md §9.");
        b.required("wound_winner_pm", i32())
            .describe("Параметр battle.wound_winner_pm; формула и единицы — army-model.md §9.");
        b.required("wound_health", i32())
            .describe("Параметр battle.wound_health; формула и единицы — army-model.md §9.");
        b.required("honor_winner", i32())
            .describe("Параметр battle.honor_winner; формула и единицы — army-model.md §9.");
        b.required("honor_loser", i32())
            .describe("Параметр battle.honor_loser; формула и единицы — army-model.md §9.");
        b.required("favor_rebels", i32())
            .describe("Параметр battle.favor_rebels; формула и единицы — army-model.md §9.");
        b.required("sigmoid", Type::list(i32()))
            .describe("Таблица battle.sigmoid; длина и порядок — army-model.md §9.");
        s.required("battle", Type::block(std::move(b)));
    }
    {
        ObjectSchema b;
        b.required("peasants_share_pm", i32())
            .describe("Параметр supply.peasants_share_pm; формула и единицы — army-model.md §9.");
        b.required("townsfolk_share_pm", i32())
            .describe("Параметр supply.townsfolk_share_pm; формула и единицы — army-model.md §9.");
        b.required("cossacks_share_pm", i32())
            .describe("Параметр supply.cossacks_share_pm; формула и единицы — army-model.md §9.");
        b.required("own_pm", i32()).describe("Параметр supply.own_pm; формула и единицы — army-model.md §9.");
        b.required("friendly_pm", i32())
            .describe("Параметр supply.friendly_pm; формула и единицы — army-model.md §9.");
        b.required("foreign_pm", i32())
            .describe("Параметр supply.foreign_pm; формула и единицы — army-model.md §9.");
        b.required("hostile_pm", i32())
            .describe("Параметр supply.hostile_pm; формула и единицы — army-model.md §9.");
        b.required("rebels_home_pm", i32())
            .describe("Параметр supply.rebels_home_pm; формула и единицы — army-model.md §9.");
        b.required("regen_x100", i32())
            .describe("Параметр supply.regen_x100; формула и единицы — army-model.md §9.");
        b.required("drain_x100", i32())
            .describe("Параметр supply.drain_x100; формула и единицы — army-model.md §9.");
        b.required("hunger_x100", i32())
            .describe("Параметр supply.hunger_x100; формула и единицы — army-model.md §9.");
        b.required("attrition_below_x100", i32())
            .describe("Параметр supply.attrition_below_x100; формула и единицы — army-model.md §9.");
        b.required("attr_max_ppm", i32())
            .describe("Параметр supply.attr_max_ppm; формула и единицы — army-model.md §9.");
        b.required("winter_pm", i32())
            .describe("Параметр supply.winter_pm; формула и единицы — army-model.md §9.");
        s.required("supply", Type::block(std::move(b)));
    }
    {
        ObjectSchema b;
        b.required("garrison_def", i32())
            .describe("Параметр siege.garrison_def; формула и единицы — army-model.md §9.");
        b.required("wall_dmg", i32())
            .describe("Параметр siege.wall_dmg; формула и единицы — army-model.md §9.");
        b.required("breach_wall", i32())
            .describe("Параметр siege.breach_wall; формула и единицы — army-model.md §9.");
        b.required("assault_min_progress", i32())
            .describe("Параметр siege.assault_min_progress; формула и единицы — army-model.md §9.");
        b.required("stores_days", i32())
            .describe("Параметр siege.stores_days; формула и единицы — army-model.md §9.");
        b.required("assault_loss_base", i32())
            .describe("Параметр siege.assault_loss_base; формула и единицы — army-model.md §9.");
        b.required("besieger_loss_pm", i32())
            .describe("Параметр siege.besieger_loss_pm; формула и единицы — army-model.md §9.");
        b.required("garrison_loss_pm", i32())
            .describe("Параметр siege.garrison_loss_pm; формула и единицы — army-model.md §9.");
        b.required("hunger_loss_pm", i32())
            .describe("Параметр siege.hunger_loss_pm; формула и единицы — army-model.md §9.");
        b.required("starving_days", i32())
            .describe("Параметр siege.starving_days; формула и единицы — army-model.md §9.");
        b.required("winter_pm", i32())
            .describe("Параметр siege.winter_pm; формула и единицы — army-model.md §9.");
        b.required("sally_share_pm", i32())
            .describe("Параметр siege.sally_share_pm; формула и единицы — army-model.md §9.");
        b.required("failed_progress", i32())
            .describe("Параметр siege.failed_progress; формула и единицы — army-model.md §9.");
        b.required("failed_morale", i32())
            .describe("Параметр siege.failed_morale; формула и единицы — army-model.md §9.");
        b.required("garrison_after_retake", i32())
            .describe("Параметр siege.garrison_after_retake; формула и единицы — army-model.md §9.");
        b.required("retake_control", i32())
            .describe("Параметр siege.retake_control; формула и единицы — army-model.md §9.");
        b.required("retake_unrest", i32())
            .describe("Параметр siege.retake_unrest; формула и единицы — army-model.md §9.");
        b.required("assault_unrest", i32())
            .describe("Параметр siege.assault_unrest; формула и единицы — army-model.md §9.");
        b.required("defence", Type::list(i32()))
            .describe("Таблица siege.defence; длина и порядок — army-model.md §9.");
        s.required("siege", Type::block(std::move(b)));
    }
    {
        ObjectSchema b;
        b.required("quality_factor", i32())
            .describe("Параметр upkeep.quality_factor; формула и единицы — army-model.md §9.");
        b.required("paid_regen_x100", i32())
            .describe("Параметр upkeep.paid_regen_x100; формула и единицы — army-model.md §9.");
        b.required("paid_supply_x100", i32())
            .describe("Параметр upkeep.paid_supply_x100; формула и единицы — army-model.md §9.");
        b.required("reliable_morale_x100", i32())
            .describe("Параметр upkeep.reliable_morale_x100; формула и единицы — army-model.md §9.");
        b.required("desert_morale_x100", i32())
            .describe("Параметр upkeep.desert_morale_x100; формула и единицы — army-model.md §9.");
        b.required("desert_morale_factor", i32())
            .describe("Параметр upkeep.desert_morale_factor; формула и единицы — army-model.md §9.");
        b.required("unreliable_months", i32())
            .describe("Параметр upkeep.unreliable_months; формула и единицы — army-model.md §9.");
        b.required("fortress", Type::list(i32()))
            .describe("Таблица upkeep.fortress; длина и порядок — army-model.md §9.");
        ObjectSchema a;
        a.required("morale", i32()).required("desert", i32());
        b.required("arrears", Type::list(Type::block(std::move(a))))
            .describe(
                "Четыре строки для 0, 1, 2, ≥3 месяцев долга; мораль в сотых, дезертирство в промилле.");
        s.required("upkeep", Type::block(std::move(b)));
    }
    {
        ObjectSchema b;
        b.required("aggr", i32()).describe("Параметр front.aggr; формула и единицы — army-model.md §9.");
        b.required("noise_x100", i32())
            .describe("Параметр front.noise_x100; формула и единицы — army-model.md §9.");
        b.required("effect", i32()).describe("Параметр front.effect; формула и единицы — army-model.md §9.");
        b.required("supply_x100", i32())
            .describe("Параметр front.supply_x100; формула и единицы — army-model.md §9.");
        b.required("call_min_size", i32())
            .describe("Параметр front.call_min_size; формула и единицы — army-model.md §9.");
        b.required("call_grace_days", i32())
            .describe("Параметр front.call_grace_days; формула и единицы — army-model.md §9.");
        b.required("high", i32()).describe("Параметр front.high; формула и единицы — army-model.md §9.");
        b.required("low", i32()).describe("Параметр front.low; формула и единицы — army-model.md §9.");
        b.required("hysteresis", i32())
            .describe("Параметр front.hysteresis; формула и единицы — army-model.md §9.");
        b.required("enemy_pull_pm", i32())
            .describe("Параметр front.enemy_pull_pm; формула и единицы — army-model.md §9.");
        b.required("peace_decay_pm", i32())
            .describe("Параметр front.peace_decay_pm; формула и единицы — army-model.md §9.");
        b.required("loss_base_ppm", i32())
            .describe("Параметр front.loss_base_ppm; формула и единицы — army-model.md §9.");
        b.required("loss_pressure_ppm", i32())
            .describe("Параметр front.loss_pressure_ppm; формула и единицы — army-model.md §9.");
        b.required("fatigue_pull_pm", i32())
            .describe("Параметр front.fatigue_pull_pm; формула и единицы — army-model.md §9.");
        b.required("fatigue_war_x100", i32())
            .describe("Параметр front.fatigue_war_x100; формула и единицы — army-model.md §9.");
        b.required("fatigue_pressure_pm", i32())
            .describe("Параметр front.fatigue_pressure_pm; формула и единицы — army-model.md §9.");
        b.required("fatigue_losses_pm", i32())
            .describe("Параметр front.fatigue_losses_pm; формула и единицы — army-model.md §9.");
        b.required("service_pm", i32())
            .describe("Параметр front.service_pm; формула и единицы — army-model.md §9.");
        b.required("honor_cap", i32())
            .describe("Параметр front.honor_cap; формула и единицы — army-model.md §9.");
        b.required("call_suspicion_x100", i32())
            .describe("Параметр front.call_suspicion_x100; формула и единицы — army-model.md §9.");
        b.required("return_days", i32())
            .describe("Параметр front.return_days; формула и единицы — army-model.md §9.");
        b.required("war_zero_months", i32())
            .describe("Параметр front.war_zero_months; формула и единицы — army-model.md §9.");
        b.required("truce_months", i32())
            .describe("Параметр front.truce_months; формула и единицы — army-model.md §9.");
        b.required("enemy_base", Type::list(i32()))
            .describe("Таблица front.enemy_base; длина и порядок — army-model.md §9.");
        s.required("front", Type::block(std::move(b)));
    }
    {
        ObjectSchema b;
        b.required("share_min", i32())
            .describe("Параметр rebels.share_min; формула и единицы — army-model.md §9.");
        b.required("share_max", i32())
            .describe("Параметр rebels.share_max; формула и единицы — army-model.md §9.");
        b.required("infantry_pm", i32())
            .describe("Параметр rebels.infantry_pm; формула и единицы — army-model.md §9.");
        b.required("cavalry_max_pm", i32())
            .describe("Параметр rebels.cavalry_max_pm; формула и единицы — army-model.md §9.");
        b.required("cavalry_noise_divisor", i32())
            .describe("Параметр rebels.cavalry_noise_divisor; формула и единицы — army-model.md §9.");
        b.required("quality_base", i32())
            .describe("Параметр rebels.quality_base; формула и единицы — army-model.md §9.");
        b.required("quality_range", i32())
            .describe("Параметр rebels.quality_range; формула и единицы — army-model.md §9.");
        b.required("dissolve_size", i32())
            .describe("Параметр rebels.dissolve_size; формула и единицы — army-model.md §9.");
        b.required("max_months", i32())
            .describe("Параметр rebels.max_months; формула и единицы — army-model.md §9.");
        b.required("decay_pm", i32())
            .describe("Параметр rebels.decay_pm; формула и единицы — army-model.md §9.");
        b.required("cooldown_days", i32())
            .describe("Параметр rebels.cooldown_days; формула и единицы — army-model.md §9.");
        b.required("spread_unrest", i32())
            .describe("Параметр rebels.spread_unrest; формула и единицы — army-model.md §9.");
        b.required("range_edges", i32())
            .describe("Параметр rebels.range_edges; формула и единицы — army-model.md §9.");
        b.required("suppress_unrest", i32())
            .describe("Параметр rebels.suppress_unrest; формула и единицы — army-model.md §9.");
        b.required("suppress_control", i32())
            .describe("Параметр rebels.suppress_control; формула и единицы — army-model.md §9.");
        b.required("unrest_start", i32())
            .describe("Параметр rebels.unrest_start; формула и единицы — army-model.md §9.");
        b.required("move_min_size", i32())
            .describe("Параметр rebels.move_min_size; формула и единицы — army-model.md §9.");
        b.required("control_monthly", i32())
            .describe("Параметр rebels.control_monthly; формула и единицы — army-model.md §9.");
        b.required("unrest_monthly", i32())
            .describe("Параметр rebels.unrest_monthly; формула и единицы — army-model.md §9.");
        b.required("cruel_loss_pm", i32())
            .describe("Параметр rebels.cruel_loss_pm; формула и единицы — army-model.md §9.");
        b.required("cruel_mood", i32())
            .describe("Параметр rebels.cruel_mood; формула и единицы — army-model.md §9.");
        b.required("capture_garrison_pm", i32())
            .describe("Параметр rebels.capture_garrison_pm; формула и единицы — army-model.md §9.");
        b.required("capture_stores_days", i32())
            .describe("Параметр rebels.capture_stores_days; формула и единицы — army-model.md §9.");
        s.required("rebels", Type::block(std::move(b)));
    }
    {
        ObjectSchema b;
        b.required("share", i32()).describe("Параметр levy.share; формула и единицы — army-model.md §9.");
        b.required("retainers", i32())
            .describe("Параметр levy.retainers; формула и единицы — army-model.md §9.");
        b.required("cavalry_pm", i32())
            .describe("Параметр levy.cavalry_pm; формула и единицы — army-model.md §9.");
        b.required("days", i32()).describe("Параметр levy.days; формула и единицы — army-model.md §9.");
        b.required("cooldown_days", i32())
            .describe("Параметр levy.cooldown_days; формула и единицы — army-model.md §9.");
        b.required("mood_nobles", i32())
            .describe("Параметр levy.mood_nobles; формула и единицы — army-model.md §9.");
        b.required("quality", i32()).describe("Параметр levy.quality; формула и единицы — army-model.md §9.");
        b.required("min_size", i32())
            .describe("Параметр levy.min_size; формула и единицы — army-model.md §9.");
        b.required("morale_x100", i32())
            .describe("Параметр levy.morale_x100; формула и единицы — army-model.md §9.");
        s.required("levy", Type::block(std::move(b)));
    }
    {
        ObjectSchema b;
        b.required("quality_new", i32())
            .describe("Параметр muster.quality_new; формула и единицы — army-model.md §9.");
        b.required("quality_trained", i32())
            .describe("Параметр muster.quality_trained; формула и единицы — army-model.md §9.");
        b.required("train_share_pm", i32())
            .describe("Параметр muster.train_share_pm; формула и единицы — army-model.md §9.");
        b.required("commander_age", i32())
            .describe("Параметр muster.commander_age; формула и единицы — army-model.md §9.");
        b.required("commander_martial", i32())
            .describe("Параметр muster.commander_martial; формула и единицы — army-model.md §9.");
        s.required("muster", Type::block(std::move(b)));
    }
    {
        ObjectSchema b;
        b.required("target_size", i32())
            .describe("Параметр guard.target_size; формула и единицы — army-model.md §9.");
        b.required("morale_slope", i32())
            .describe("Параметр guard.morale_slope; формула и единицы — army-model.md §9.");
        b.required("pay_step_x100", i32())
            .describe("Параметр guard.pay_step_x100; формула и единицы — army-model.md §9.");
        b.required("shift_unreliable", i32())
            .describe("Параметр guard.shift_unreliable; формула и единицы — army-model.md §9.");
        b.required("shift_destroyed", i32())
            .describe("Параметр guard.shift_destroyed; формула и единицы — army-model.md §9.");
        b.required("morale_after_coup", i32())
            .describe("Параметр guard.morale_after_coup; формула и единицы — army-model.md §9.");
        b.required("morale_pivot_x100", i32())
            .describe("Параметр guard.morale_pivot_x100; формула и единицы — army-model.md §9.");
        b.required("absent_x100", i32())
            .describe("Параметр guard.absent_x100; формула и единицы — army-model.md §9.");
        s.required("guard", Type::block(std::move(b)));
    }
    {
        ObjectSchema b;
        b.required("attack_min_pm", i32())
            .describe("Параметр ai.attack_min_pm; формула и единицы — army-model.md §9.");
        b.required("assault_min_pm", i32())
            .describe("Параметр ai.assault_min_pm; формула и единицы — army-model.md §9.");
        b.required("front_suspicion", i32())
            .describe("Параметр ai.front_suspicion; формула и единицы — army-model.md §9.");
        b.required("front_favor", i32())
            .describe("Параметр ai.front_favor; формула и единицы — army-model.md §9.");
        s.required("ai", Type::block(std::move(b)));
    }
    ObjectSchema branch;
    for (const auto* name :
         {"cv", "speed_kmd", "upkeep_kop", "equip_kop", "vuln", "need", "siege_value", "train_days"})
        branch.required(name, i32());
    s.repeatable_labeled("branch", Type::block(std::move(branch)))
        .describe("Ровно четыре рода: infantry, cavalry, artillery, irregular; скорость — км/день, деньги — "
                  "копейки/человека, обучение — дни, остальные веса — промилле.");
    ObjectSchema terrain;
    terrain.required("move_pm", i32())
        .required("defence_pm", i32())
        .required("supply_pm", i32())
        .required("class", Type::enumeration({"open", "rough"}));
    s.repeatable_labeled("terrain", Type::block(std::move(terrain)))
        .describe(
            "Ровно шесть местностей: plain, steppe, forest, swamp, hills, mountain; множители в промилле.");
    ObjectSchema season;
    season.required("month", Type::integer(1, 12)).required("move_pm", i32()).required("supply_pm", i32());
    s.repeatable("season", Type::block(std::move(season)))
        .describe("Ровно одна запись каждого месяца 1…12; множители движения и снабжения в промилле.");
    ObjectSchema counter;
    for (const auto* cls : {"open", "rough"}) {
        ObjectSchema rows;
        for (const auto* own : {"infantry", "cavalry", "artillery", "irregular"}) {
            ObjectSchema cols;
            for (const auto* enemy : {"infantry", "cavalry", "artillery", "irregular"})
                cols.required(enemy, i32());
            rows.required(own, Type::block(std::move(cols)));
        }
        counter.required(cls, Type::block(std::move(rows)));
    }
    s.required("counter", Type::block(std::move(counter)))
        .describe(
            "Две полные матрицы 4×4 в промилле: строка — свой род, столбец — противник; 1000 нейтрален.");
    return s;
}
ObjectSchema military_schema() {
    ObjectSchema s("military_setup");
    s.intern_last().describe_kind("Изолированная военная стартовая конфигурация: ссылки на существующие "
                                  "province/dynasty; не применяется к World до E16-T20. military.pd пока "
                                  "синтетический пример, исторический roster задаёт E30.");
    s.required("capital", Type::reference("province"))
        .describe("Столица сценария и место гвардии.")
        .required("crown_manpower", Type::integer(0, 4000000))
        .describe("Начальный рекрутский резерв короны, людей.");
    ObjectSchema fort;
    fort.required("level", Type::integer(0, 5))
        .required("garrison", Type::integer(0, 40000))
        .required("stores_days", Type::integer(0, 3650))
        .required("wall_pm", Type::integer(0, 1000))
        .required("holder", Type::enumeration({"owner", "rebels"}));
    ObjectSchema province;
    province.required("id", Type::reference("province"))
        .required("terrain", Type::enumeration({"plain", "steppe", "forest", "swamp", "hills", "mountain"}))
        .optional("fortress", Type::block(std::move(fort)));
    s.repeatable("province", Type::block(std::move(province)))
        .describe("Ровно одна местность на провинцию; крепость необязательна, гарнизон — люди, запасы — дни, "
                  "стены — промилле.");
    ObjectSchema comp;
    for (const auto* name : {"infantry", "cavalry", "artillery", "irregular"})
        comp.required(name, Type::integer(0, 40000));
    ObjectSchema army;
    army.required("name_key", Type::string())
        .required("owner", Type::enumeration({"crown", "dynasty", "rebels"}))
        .optional("dynasty", Type::reference("dynasty"))
        .required("tag", Type::enumeration({"regular", "levy", "guard", "insurgents"}))
        .required("province", Type::reference("province"))
        .required("comp", Type::block(std::move(comp)))
        .required("quality", Type::integer(0, 100))
        .required("experience", Type::integer(0, 100))
        .required("morale_x100", Type::integer(0, 10000))
        .required("supply_x100", Type::integer(0, 10000))
        .optional("commander_office",
                  Type::enumeration({"general", "guards_colonel", "fortress_commandant"}));
    s.repeatable_labeled("army", Type::block(std::move(army)))
        .describe("Стартовые армии, целые люди по четырём родам; dynasty только при owner=династия, гвардия "
                  "только у короны в столице. commander_office — символический запрос, не выдуманный "
                  "персонаж; назначает T13/T20.");
    ObjectSchema front;
    front.required("id", Type::identifier())
        .required("name_key", Type::string())
        .required("state", Type::enumeration({"peace", "tension", "war", "truce"}))
        .required("pressure_x100", Type::integer(0, 10000))
        .required("enemy_x100", Type::integer(0, 10000))
        .required("fatigue_x100", Type::integer(0, 10000));
    s.required("front", Type::block(std::move(front)))
        .describe("Единственный фронт M1 (north), давление, противник и усталость — сотые 0…100.");
    return s;
}
}  // namespace
void add_army_schemas(SchemaSet& set) {
    set.add(rules_schema());
    set.add(military_schema());
}
}  // namespace pat::data
