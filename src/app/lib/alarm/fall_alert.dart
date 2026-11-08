/// Parsed representation of a fall-alert push, built from the FCM `data` map.
///
/// The field names here MUST match the Cloud Function payload
/// (`src/app/functions/src/index.ts`) and the "Notification data contract" in
/// DATA_MODEL.md: `eventId`, `fallerId`, `fallerName`, `deviceId`, `type`, `ts`.
/// FCM delivers every data value as a string, so `ts` arrives as ISO-8601.
class FallAlert {
  const FallAlert({
    this.eventId,
    this.fallerId,
    required this.fallerName,
    this.deviceId,
    this.type,
    this.ts,
  });

  /// `events/{eventId}` doc id. Absent for manual/console test pushes — the
  /// acknowledge action must no-op gracefully when this is null/empty.
  final String? eventId;

  /// UID of the faller (== event.userId).
  final String? fallerId;

  /// Denormalized display name for the alert text. Defaults to "Someone".
  final String fallerName;

  /// The device that fired.
  final String? deviceId;

  /// Event type enum, e.g. "fall_confirmed".
  final String? type;

  /// Server time the event was recorded, parsed from the ISO-8601 `ts` string.
  final DateTime? ts;

  bool get hasEventId => (eventId != null && eventId!.isNotEmpty);

  /// Builds a [FallAlert] from an FCM message `data` map (all values strings).
  factory FallAlert.fromData(Map<String, dynamic> data) {
    String? str(String key) {
      final value = data[key];
      if (value == null) return null;
      final s = value.toString();
      return s.isEmpty ? null : s;
    }

    final tsRaw = str('ts');
    return FallAlert(
      eventId: str('eventId'),
      fallerId: str('fallerId'),
      fallerName: str('fallerName') ?? 'Someone',
      deviceId: str('deviceId'),
      type: str('type'),
      ts: tsRaw == null ? null : DateTime.tryParse(tsRaw),
    );
  }

  /// Serializes back to a string map, used as the local-notification payload so
  /// a foreground notification tap can re-open the same alarm.
  Map<String, String> toData() {
    final data = <String, String>{'fallerName': fallerName};
    void put(String key, String? value) {
      if (value != null) data[key] = value;
    }

    put('eventId', eventId);
    put('fallerId', fallerId);
    put('deviceId', deviceId);
    put('type', type);
    put('ts', ts?.toIso8601String());
    return data;
  }
}
