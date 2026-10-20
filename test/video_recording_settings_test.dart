import 'package:flutter_test/flutter_test.dart';
import 'package:speakr_app/api/models.dart';
import 'package:speakr_app/features/live/recording_state.dart';
import 'package:speakr_app/services/auto_record/auto_record_settings.dart';

void main() {
  group('video settings', () {
    test('defaultVideoEnabled is off and round-trips through JSON', () {
      expect(const AutoRecordSettings().defaultVideoEnabled, isFalse);
      final s = AutoRecordSettings.fromJson(
          const AutoRecordSettings(defaultVideoEnabled: true).toJson());
      expect(s.defaultVideoEnabled, isTrue);
    });

    test('settings saved before video existed decode with video off', () {
      final s = AutoRecordSettings.fromJson(const {'enabled': true});
      expect(s.defaultVideoEnabled, isFalse);
    });

    test('per-app videoEnabled: explicit false survives, null is dropped', () {
      const entry = AllowlistEntry(
        key: 'Teams.exe',
        displayName: 'Teams',
        kind: AllowlistKind.exeBasename,
      );
      expect(entry.toJson().containsKey('videoEnabled'), isFalse);
      final off = AllowlistEntry.fromJson(
          entry.copyWith(videoEnabled: false).toJson());
      expect(off.videoEnabled, isFalse);
      expect(off.copyWith(videoEnabled: null).videoEnabled, isNull);
      expect(off.copyWith(speakers: 3).videoEnabled, isFalse);
    });
  });

  group('RecordingState video fields', () {
    test('decode to defaults from older mini-window JSON', () {
      final s = RecordingState.fromJson(const {'started': true});
      expect(s.videoSupported, isFalse);
      expect(s.videoRecording, isFalse);
      expect(s.videoWindowHwnd, isNull);
      expect(s.videoWindowTitle, isNull);
    });

    test('round-trip through JSON', () {
      const s = RecordingState(
        videoSupported: true,
        videoRecording: true,
        videoWindowHwnd: 0x1234,
        videoWindowTitle: 'Weekly sync | Microsoft Teams',
      );
      expect(RecordingState.fromJson(s.toJson()), s);
    });
  });

  group('Config', () {
    test('parses video_retention and max_file_size_mb', () {
      final c = Config.fromJson(const {
        'video_retention': true,
        'max_file_size_mb': 250,
      });
      expect(c.videoRetention, isTrue);
      expect(c.maxFileSizeMb, 250);
    });

    test('older servers without the keys mean no video retention', () {
      final c = Config.fromJson(const <String, dynamic>{});
      expect(c.videoRetention, isFalse);
      expect(c.maxFileSizeMb, isNull);
    });
  });
}
