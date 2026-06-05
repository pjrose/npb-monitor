from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Any


@dataclass(frozen=True, slots=True)
class ChargeProfile:
    id: str
    label: str
    chemistry: str
    nominal_voltage: str
    description: str
    cc_current_a: float
    cv_voltage_v: float
    float_voltage_v: float
    taper_current_a: float
    restart_voltage_v: float
    notes: list[str]
    sources: list[str]

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


PROFILES: dict[str, ChargeProfile] = {
    "lifepo4_24v": ChargeProfile(
        id="lifepo4_24v",
        label="LiFePO4 24V (8S)",
        chemistry="LiFePO4",
        nominal_voltage="24V",
        description="Conservative 8S LiFePO4 profile for a BMS-protected 24V nominal pack.",
        cc_current_a=5.0,
        cv_voltage_v=28.4,
        float_voltage_v=27.0,
        taper_current_a=0.5,
        restart_voltage_v=26.4,
        notes=[
            "Use the battery/BMS datasheet first. 0.5C is a common LiFePO4 upper recommendation, but this profile stages 5A until capacity is known.",
            "Do not charge LiFePO4 below the battery manufacturer's low-temperature limit unless the pack explicitly supports it.",
            "Float is set low for standby; many LiFePO4 systems prefer no float or a low standby float.",
        ],
        sources=[
            "https://www.victronenergy.com/media/pg/Lithium_NG_battery_51%2C2_V/en/operation.html",
            "https://www.victronenergy.nl/media/pg/Lithium_Battery_Smart/en/operation.html",
        ],
    ),
    "lead_acid_24v": ChargeProfile(
        id="lead_acid_24v",
        label="Lead acid 24V",
        chemistry="Lead acid",
        nominal_voltage="24V",
        description="General-purpose sealed/AGM-style 24V lead-acid profile at roughly room temperature.",
        cc_current_a=4.0,
        cv_voltage_v=28.8,
        float_voltage_v=27.2,
        taper_current_a=0.55,
        restart_voltage_v=26.4,
        notes=[
            "Lead-acid settings vary strongly by flooded/AGM/gel construction and temperature compensation.",
            "This stages 4A as a conservative starting current. Many lead-acid makers specify charge current as a percentage of C20 capacity.",
            "Flooded deep-cycle batteries may call for higher absorption voltage than this profile; check the battery datasheet.",
        ],
        sources=[
            "https://www.trojanbattery.com/pages/technical-support/battery-maintenance",
            "https://rollsbattery.com/wp-content/uploads/2019/11/Flooded-Charging-Parameters.pdf",
        ],
    ),
}


def profile_payload() -> list[dict[str, Any]]:
    return [profile.to_dict() for profile in PROFILES.values()]
