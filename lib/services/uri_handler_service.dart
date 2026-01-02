import 'dart:async';
import 'dart:io';

import 'package:flutter/services.dart';

import '../utils/app_logger.dart';

typedef UriCallback = void Function(Uri uri);

class UriHandlerService {
  static const String _channelName = 'com.edde746.plezy/uri';
  static final MethodChannel _channel = const MethodChannel(_channelName);

  static UriHandlerService? _instance;
  static UriHandlerService get instance {
    _instance ??= UriHandlerService._();
    return _instance!;
  }

  UriCallback? _callback;
  final List<Uri> _pendingUris = [];
  bool _isInitialized = false;

  UriHandlerService._();

  static bool get isAvailable {
    return Platform.isLinux || Platform.isMacOS || Platform.isWindows;
  }

  void initialize() {
    if (!isAvailable || _isInitialized) return;
    _isInitialized = true;

    _channel.setMethodCallHandler(_handleMethodCall);
    appLogger.i('URI handler service initialized');
  }

  void setCallback(UriCallback callback) {
    _callback = callback;

    for (final uri in _pendingUris) {
      _callback?.call(uri);
    }
    _pendingUris.clear();
  }

  void clearCallback() {
    _callback = null;
  }

  Future<dynamic> _handleMethodCall(MethodCall call) async {
    if (call.method == 'onUri') {
      final uriString = call.arguments as String?;
      if (uriString != null) {
        _handleUri(uriString);
      }
    }
    return null;
  }

  void _handleUri(String uriString) {
    appLogger.i('Received URI: $uriString');

    try {
      final uri = Uri.parse(uriString);
      if (uri.scheme != 'plezy') {
        appLogger.w('Ignoring non-plezy URI: $uriString');
        return;
      }

      if (_callback != null) {
        _callback!(uri);
      } else {
        _pendingUris.add(uri);
        appLogger.d('URI queued (no callback registered yet)');
      }
    } catch (e) {
      appLogger.e('Failed to parse URI: $uriString', error: e);
    }
  }

  static Uri? parsePlayUri(Uri uri) {
    if (uri.scheme != 'plezy') return null;
    if (uri.host != 'play' && uri.host != 'preplay') return null;
    return uri;
  }

  static String? extractRatingKey(Uri uri) {
    final key = uri.queryParameters['key'];
    if (key == null) return null;

    final match = RegExp(r'/library/metadata/(\d+)').firstMatch(key);
    return match?.group(1);
  }

  static bool isPreplay(Uri uri) {
    return uri.host == 'preplay';
  }

  void dispose() {
    _callback = null;
    _pendingUris.clear();
    _isInitialized = false;
  }
}
