// The built-in model library's table (see models.hpp): the curated
// .model / .subckt cards, shelf order - diodes, BJTs, MOSFETs, JFETs,
// switches, opamps. Pure data; the lookup lives here too.
//
// Curation rules, so the shelf never ships a card the engine cannot
// back: every parameter is one the device's apply_model IMPLEMENTS
// (the accepted-but-ignored vendor tail is left out on purpose - a
// library card is a promise, not a wish), every value is the
// datasheet-typical terminal behavior fit onto the implemented level
// (the Level-1 MOSFETs spell their geometry story in the desc: the
// card is tuned for the default W/L=1 and scales with the part's own
// L=/W=), and the opamps are LINEAR macromodels - E/R/C only, the
// supplies are sense pins - because a linear stage needs no .model
// inside the subckt body (the flattener keeps subckt bodies device-
// cards-only by design). The polarity signs follow the engine's own
// mirror-frame conventions: PMOS Vto positive for enhancement, PJF
// Vto negative (the NJF sign), like the §10.1 rows.

#include <string>
#include <vector>

#include <ws/circsim/models.hpp>

#include "util.hpp"  // lower (the locale-free ASCII one)

namespace ws::circsim {

namespace {

ModelCard card(std::string name, std::string title, std::string kind,
               std::string text, std::string desc) {
    ModelCard c;
    c.name = std::move(name);
    c.title = std::move(title);
    c.kind = std::move(kind);
    c.text = std::move(text);
    c.desc = std::move(desc);
    return c;
}

const std::vector<ModelCard>& table() {
    static const std::vector<ModelCard> models = {
        // --- diodes ------------------------------------------------------
        card("1n4148", "1N4148", "d",
             ".model 1n4148 D(Is=4.35n N=1.6666 Rs=0.8456 Tt=4.4n"
             " Cjo=1.1p M=0.23 Vj=0.75 Bv=100 Ibv=100u)",
             "small-signal switching diode (100 V, ~0.65 V @ 10 mA)"),
        card("1n4007", "1N4007", "d",
             ".model 1n4007 D(Is=176p N=1.3 Rs=45m Tt=4.32u"
             " Cjo=39.8p M=0.375 Vj=0.75 Bv=1100 Ibv=10u)",
             "1 A rectifier (1000 V; ~0.6 V @ 10 mA, ~0.8 V @ 1 A)"),
        card("bat54", "BAT54", "d",
             ".model bat54 D(Is=195n N=1.1 Rs=0.8 Tt=5n Cjo=10p M=0.27"
             " Vj=0.75 Bv=30 Ibv=100u)",
             "30 V Schottky (~0.25 V @ 1 mA)"),
        card("1n4733", "1N4733", "d",
             ".model 1n4733 D(Is=1p N=1.2 Rs=0.6 Bv=5.1 Ibv=49m)",
             "5.1 V Zener, 1 W (Izt 49 mA)"),
        card("led_red", "Red LED", "d",
             ".model led_red D(Is=1e-15 N=2.5 Rs=1 Cjo=20p)",
             "red LED, ~1.9 V @ 10 mA"),
        card("led_green", "Green LED", "d",
             ".model led_green D(Is=1e-15 N=2.9 Rs=1 Cjo=20p)",
             "green LED, ~2.3 V @ 20 mA"),
        card("led_blue", "Blue LED", "d",
             ".model led_blue D(Is=1e-15 N=3.8 Rs=1 Cjo=20p)",
             "blue LED, ~3.0 V @ 20 mA"),
        // --- BJTs (Gummel-Poon core: Is/Bf/Br/Vaf/Var + the junction
        // caps and transit times; the accepted-but-ignored high-
        // injection tail stays out - the card is what the engine runs)
        card("2n2222", "2N2222", "npn",
             ".model 2n2222 NPN(Is=3.88e-14 Bf=220 Br=4 Vaf=128"
             " Cje=25p Cjc=8p Vje=0.75 Vjc=0.75 Mje=0.33 Mjc=0.33"
             " Tf=411p Tr=46n)",
             "NPN small-signal (Vbe ~0.62 V @ 1 mA)"),
        card("2n3904", "2N3904", "npn",
             ".model 2n3904 NPN(Is=6.734f Bf=206 Br=4 Vaf=74"
             " Cje=4.5p Cjc=3.6p Vje=0.75 Vjc=0.75 Mje=0.33 Mjc=0.33"
             " Tf=350p Tr=240n)",
             "NPN general purpose (40 V, 200 mA)"),
        card("bc547", "BC547", "npn",
             ".model bc547 NPN(Is=2f Bf=330 Br=6 Vaf=110"
             " Cje=12p Cjc=4.5p Vje=0.75 Vjc=0.75 Mje=0.33 Mjc=0.33"
             " Tf=450p Tr=200n)",
             "NPN small-signal (45 V, 100 mA, high beta)"),
        card("2n3906", "2N3906", "pnp",
             ".model 2n3906 PNP(Is=1.41e-14 Bf=180 Br=4 Vaf=100"
             " Cje=10p Cjc=4.5p Vje=0.75 Vjc=0.75 Mje=0.33 Mjc=0.33"
             " Tf=350p Tr=240n)",
             "PNP general purpose (40 V, 200 mA)"),
        card("bc557", "BC557", "pnp",
             ".model bc557 PNP(Is=2f Bf=330 Br=6 Vaf=110"
             " Cje=10p Cjc=4.5p Vje=0.75 Vjc=0.75 Mje=0.33 Mjc=0.33"
             " Tf=450p Tr=200n)",
             "PNP small-signal (45 V, 100 mA, high beta)"),
        // --- MOSFETs (Level 1, Shichman-Hodges; tuned at the default
        // geometry (no L=/W= on the card), so the part's own L=/W=
        // scales the channel - Rds(on) at the datasheet drive follows)
        card("2n7000", "2N7000", "nmos",
             ".model 2n7000 NMOS(Vto=2 Kp=0.08)",
             "NMOS small-signal, 60 V (~2 R on at Vgs=10; set W/L to scale)"),
        card("irf540", "IRF540", "nmos",
             ".model irf540 NMOS(Vto=3.5 Kp=3.5)",
             "NMOS power, 100 V / 33 A (44 mR on at Vgs=10)"),
        card("bs250", "BS250", "pmos",
             ".model bs250 PMOS(Vto=1.8 Kp=0.12)",
             "PMOS small-signal, -45 V (gate below source to conduct)"),
        card("irf9540", "IRF9540", "pmos",
             ".model irf9540 PMOS(Vto=3.5 Kp=1.3)",
             "PMOS power, -100 V / -23 A (117 mR on at Vgs=-10)"),
        // --- JFETs (Beta*Vto^2 = Idss; the PJF keeps the NJF's sign)
        card("j2n5457", "J2N5457", "njf",
             ".model j2n5457 NJF(Vto=-2 Beta=1e-4)",
             "N-channel JFET (Idss 0.4 mA)"),
        card("j2n5459", "J2N5459", "njf",
             ".model j2n5459 NJF(Vto=-3 Beta=1e-3)",
             "N-channel JFET (Idss 9 mA)"),
        card("2n5460", "2N5460", "pjf",
             ".model 2n5460 PJF(Vto=-1.5 Beta=1e-3)",
             "P-channel JFET (Idss 2.3 mA)"),
        // --- switches (one SW card serves both: S reads Vt/Vh, W reads
        // It/Ih - a card carrying both works for either symbol)
        card("sw_ideal", "Ideal switch", "sw",
             ".model sw_ideal SW(Ron=1 Roff=1meg Vt=0 Vh=0 It=1m Ih=0)",
             "closes above 0 V (S) or 1 mA (W)"),
        card("sw_ttl", "TTL switch", "sw",
             ".model sw_ttl SW(Ron=10 Roff=1meg Vt=2.5 Vh=0.4"
             " It=2.5m Ih=0.4m)",
             "logic-level: closes above ~2.5 V, 0.4 hysteresis"),
        // --- opamps (linear macromodels: Avol, the GBW pole, Ri, Ro;
        // the supplies are sense pins - the linear stage never rails).
        // Pole = 1/(2pi * 1meg * Cg); GBW = Avol * pole.
        //   opamp: Avol 1e6, 10 Hz pole  -> 10 MHz GBW
        //   ua741: Avol 2e5, 5 Hz pole   -> 1 MHz GBW (Ri 2meg, Ro 75)
        //   lm358: Avol 1e5, 10 Hz pole  -> 1 MHz GBW
        //   tl071: Avol 2e5, 15 Hz pole  -> 3 MHz GBW (JFET input)
        [] {
            ModelCard c;
            c.name = "opamp";
            c.title = "Ideal op amp";
            c.kind = "opamp";
            c.text =
                ".subckt opamp out inp inn vp vm\n"
                "Ri inp inn 1e12\n"
                "Eg g1 0 inp inn 1\n"
                "Rg g1 g2 1meg\n"
                "Cg g2 0 15.9n\n"
                "Eo b1 0 g2 0 1e6\n"
                "Ro b1 out 1\n"
                ".ends";
            c.desc = "ideal: 1e6 gain, 10 MHz GBW, near-zero out R";
            return c;
        }(),
        [] {
            ModelCard c;
            c.name = "ua741";
            c.title = "uA741";
            c.kind = "opamp";
            c.text =
                ".subckt ua741 out inp inn vp vm\n"
                "Ri inp inn 2meg\n"
                "Eg g1 0 inp inn 1\n"
                "Rg g1 g2 1meg\n"
                "Cg g2 0 31.8n\n"
                "Eo b1 0 g2 0 2e5\n"
                "Ro b1 out 75\n"
                ".ends";
            c.desc = "bipolar classic: 200k gain, 1 MHz GBW, 2 Meg in";
            return c;
        }(),
        [] {
            ModelCard c;
            c.name = "lm358";
            c.title = "LM358";
            c.kind = "opamp";
            c.text =
                ".subckt lm358 out inp inn vp vm\n"
                "Ri inp inn 1meg\n"
                "Eg g1 0 inp inn 1\n"
                "Rg g1 g2 1meg\n"
                "Cg g2 0 15.9n\n"
                "Eo b1 0 g2 0 1e5\n"
                "Ro b1 out 150\n"
                ".ends";
            c.desc = "bipolar single-supply: 100k gain, 1 MHz GBW";
            return c;
        }(),
        [] {
            ModelCard c;
            c.name = "tl071";
            c.title = "TL071";
            c.kind = "opamp";
            c.text =
                ".subckt tl071 out inp inn vp vm\n"
                "Ri inp inn 1e12\n"
                "Eg g1 0 inp inn 1\n"
                "Rg g1 g2 1meg\n"
                "Cg g2 0 10.6n\n"
                "Eo b1 0 g2 0 2e5\n"
                "Ro b1 out 100\n"
                ".ends";
            c.desc = "JFET input: 200k gain, 3 MHz GBW";
            return c;
        }(),
    };
    return models;
}

}  // namespace

const std::vector<ModelCard>& builtin_models() {
    return table();
}

const ModelCard* find_builtin_model(std::string_view name) {
    const std::string key = lower(name);
    for (const ModelCard& c : table()) {
        if (key == c.name) {
            return &c;
        }
    }
    return nullptr;
}

std::vector<const ModelCard*> builtin_models_of_kind(std::string_view kind) {
    const std::string key = lower(kind);
    std::vector<const ModelCard*> out;
    for (const ModelCard& c : table()) {
        if (key == c.kind) {
            out.push_back(&c);
        }
    }
    return out;
}

std::string model_kind_label(std::string_view kind) {
    const std::string k{kind};
    if (k == "d") {
        return "diode";
    }
    if (k == "npn") {
        return "NPN transistor";
    }
    if (k == "pnp") {
        return "PNP transistor";
    }
    if (k == "nmos") {
        return "N-channel MOSFET";
    }
    if (k == "pmos") {
        return "P-channel MOSFET";
    }
    if (k == "njf") {
        return "N-channel JFET";
    }
    if (k == "pjf") {
        return "P-channel JFET";
    }
    if (k == "sw") {
        return "switch";
    }
    if (k == "opamp") {
        return "op amp";
    }
    return "model";
}

}  // namespace ws::circsim
