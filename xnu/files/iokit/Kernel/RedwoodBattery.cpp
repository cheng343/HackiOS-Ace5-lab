/*
 * RedwoodBattery: the phone's battery as iOS looks for it, an IOPMPowerSource.
 *
 * powerd finds every service of the class IOPMPowerSource (it calls the first "InternalBattery-0"),
 * listens for its kIOPMMessageBatteryStatusHasChanged and then reads its properties: whether a
 * battery is installed, CurrentCapacity and MaxCapacity (without both it gives up on the battery),
 * whether a charger is connected and charging, voltage, current, cycles, the charger's details.
 * From them it makes the power sources list (IOPSCopyPowerSourcesInfo) and posts its notifications,
 * and everything else follows from that: the status bar's battery, the lock screen's "Charging",
 * the Batteries widget, Settings > Battery.
 *
 * What the values are comes from the audio DSP's battery manager (bsd/dev/redwood_adsp.c,
 * with adsp=1, unless battmgr=0), by way of bsd/dev/redwood_battery.c, which calls
 * RedwoodBatteryReport() with every reading; the first one makes the service. Nothing here asks
 * the charger anything or tells it anything.
 *
 * As on Apple's newer batteries, CurrentCapacity is the percentage that the fuel gauge shows (and
 * MaxCapacity 100); the charges in mAh are AppleRawCurrentCapacity, AppleRawMaxCapacity (what the
 * gauge has learnt a full charge to be), NominalChargeCapacity and DesignCapacity.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
/* This private shared header is not part of the exported IOKit headers. */
#include "../../bsd/sys/redwood_battery.h"

extern "C" void RedwoodBatteryReport(const void * state, unsigned size);

class RedwoodBattery : public IOPMPowerSource
{
	OSDeclareDefaultStructors(RedwoodBattery);

public:
	void report(const struct redwood_battery_state * b);

private:
	void number(const char * key, long long value);
	void flag(const char * key, bool value);
	void object(const char * key, OSObject * value);
	void forget(const char * key);
};

OSDefineMetaClassAndStructors(RedwoodBattery, IOPMPowerSource);

static RedwoodBattery * battery;

void
RedwoodBattery::number(const char * key, long long value)
{
	const OSSymbol * symbol = OSSymbol::withCString(key);
	OSNumber * n = OSNumber::withNumber((unsigned long long) value, 32);

	if (symbol != NULL && n != NULL) {
		setPSProperty(symbol, n);
	}
	OSSafeReleaseNULL(n);
	OSSafeReleaseNULL(symbol);
}

void
RedwoodBattery::flag(const char * key, bool value)
{
	const OSSymbol * symbol = OSSymbol::withCString(key);

	if (symbol != NULL) {
		setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
	}
	OSSafeReleaseNULL(symbol);
}

void
RedwoodBattery::object(const char * key, OSObject * value)
{
	const OSSymbol * symbol = OSSymbol::withCString(key);

	if (symbol != NULL && value != NULL) {
		setPSProperty(symbol, value);
	}
	OSSafeReleaseNULL(symbol);
}

/* Unknown is absence, not a false charger reading retained from another source.
 * IOPMPowerSource::updateStatus copies its protected properties dictionary, so
 * remove from that dictionary and the published IORegistry property together. */
void
RedwoodBattery::forget(const char * key)
{
	if (properties->getObject(key) != NULL) {
		properties->removeObject(key);
		settingsChangedSinceUpdate = true;
	}
	removeProperty(key);
}

/*
 * What iOS calls the charger by (IOPSKeysPrivate.h's kIOPSFamilyCode*, iokit_family_err(sub_iokit_usb, n)),
 * from Linux's POWER_SUPPLY_USB_TYPE_*, as the DSP gives it.
 */
static unsigned
family_code(int usb_type)
{
	switch (usb_type) {
	case 1: return 0xe0004000;      /* SDP, a computer's port: kIOPSFamilyCodeUSBHost */
	case 2: return 0xe0004004;      /* DCP, a charger: kIOPSFamilyCodeUSBChargingPortDedicated */
	case 3: return 0xe0004005;      /* CDP, a computer's charging port: kIOPSFamilyCodeUSBChargingPortDownstream */
	case 5: return 0xe0004009;      /* USB-C: kIOPSFamilyCodeUSBCTypeC */
	case 6: case 7: case 8: return 0xe000400a;      /* USB Power Delivery: kIOPSFamilyCodeUSBCPD */
	default: return 0xe0004007;     /* kIOPSFamilyCodeUSBUnknown */
	}
}

void
RedwoodBattery::report(const struct redwood_battery_state * b)
{
	int percent = redwood_battery_round_hundredths(b->percent_hundredths);
	bool charging = redwood_battery_is_charging(b);

	setBatteryInstalled(b->present != 0);
	if (b->external >= 0) {
		setExternalConnected(b->external != 0);
		setExternalChargeCapable(b->external != 0);
		setIsCharging(charging);
		flag("FullyCharged", b->external && (b->status == 4 || percent == 100));
		flag("AppleRawExternalConnected", b->external != 0);
	} else {
		forget(kIOPMPSExternalConnectedKey);
		forget(kIOPMPSExternalChargeCapableKey);
		forget(kIOPMPSIsChargingKey);
		forget("FullyCharged");
		forget("AppleRawExternalConnected");
	}
	setCurrentCapacity((unsigned) percent);
	setMaxCapacity(100);
	setVoltage((unsigned) (b->voltage_uv / 1000));
	setAmperage(b->current_ua / 1000);
	number("InstantAmperage", b->current_ua / 1000);
	number("Temperature", b->temperature_hundredths);
	if (b->charge_uah > 0) {
		number("AppleRawCurrentCapacity", b->charge_uah / 1000);
	}
	if (b->full_uah > 0) {
		number("AppleRawMaxCapacity", b->full_uah / 1000);
		number("NominalChargeCapacity", b->full_uah / 1000);
	}
	if (b->design_uah > 0) {
		number("DesignCapacity", b->design_uah / 1000);
	}
	if (b->cycles > 0) {
		setCycleCount((unsigned) b->cycles);
	}

	if (b->external == 1) {
		OSDictionary * adapter = OSDictionary::withCapacity(6);
		long long mv = b->usb_voltage_uv / 1000, ma = b->usb_current_ua / 1000;
		OSNumber * n;

		if (adapter != NULL) {
			if ((n = OSNumber::withNumber((unsigned long long) ((mv * ma + 500000) / 1000000), 32)) != NULL) {
				adapter->setObject(kIOPMPSAdapterDetailsWattsKey, n);
				n->release();
			}
			if ((n = OSNumber::withNumber((unsigned long long) ma, 32)) != NULL) {
				adapter->setObject(kIOPMPSAdapterDetailsAmperageKey, n);
				n->release();
			}
			if ((n = OSNumber::withNumber((unsigned long long) mv, 32)) != NULL) {
				adapter->setObject(kIOPMPSAdapterDetailsVoltage, n);
				n->release();
			}
			if ((n = OSNumber::withNumber((unsigned long long) family_code(b->usb_type), 32)) != NULL) {
				adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, n);
				n->release();
			}
			adapter->setObject("IsWireless", kOSBooleanFalse);
			object(kIOPMPSAdapterDetailsKey, adapter);
			adapter->release();
		}
	} else if (b->external == 0) {
		OSDictionary * none = OSDictionary::withCapacity(1);
		OSNumber * n = OSNumber::withNumber(0ULL, 32);    /* kIOPSFamilyCodeDisconnected */

		if (none != NULL && n != NULL) {
			none->setObject(kIOPMPSAdapterDetailsFamilyKey, n);
			object(kIOPMPSAdapterDetailsKey, none);
		}
		OSSafeReleaseNULL(n);
		OSSafeReleaseNULL(none);
	} else {
		forget(kIOPMPSAdapterDetailsKey);
	}
	updateStatus();
}

/* With every reading of the battery (bsd/dev/redwood_battery.c); the first one makes the service. One caller at a time. */
void
RedwoodBatteryReport(const void * state, unsigned size)
{
	const struct redwood_battery_state * b = (const struct redwood_battery_state *) state;
	IOService * platform;

	if (b == NULL || size != sizeof(struct redwood_battery_state) || b->size != sizeof(struct redwood_battery_state)) {
		static bool said;

		if (!said) {
			said = true;
			IOLog("RedwoodBattery: missing or incompatible battery state (%u bytes, expected %lu)\n",
			    size, sizeof(struct redwood_battery_state));
		}
		return;
	}
	if (!redwood_battery_state_valid(b)) {
		return;
	}
	if (battery == NULL) {
		RedwoodBattery * made = OSTypeAlloc(RedwoodBattery);

		if ((platform = IOService::getPlatform()) == NULL || made == NULL || !made->init()) {
			OSSafeReleaseNULL(made);
			return;
		}
		made->report(b);        /* (the properties are there before anybody finds the service) */
		if (!made->attach(platform)) {
			made->release();
			return;
		}
		made->setName("RedwoodBattery");
		if (!made->start(platform)) {
			made->detach(platform);
			made->release();
			return;
		}
		made->registerService();
		battery = made;
		IOLog("RedwoodBattery: the battery's power source is up: %d%%, %s\n", redwood_battery_round_hundredths(b->percent_hundredths),
		    b->external < 0 ? "charger state unknown" :
		    b->external ? (redwood_battery_is_charging(b) ? "charging" : "on a charger, not charging") : "not on a charger");
		return;
	}
	battery->report(b);
}
