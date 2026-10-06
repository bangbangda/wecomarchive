<?php
if (!extension_loaded('wecomarchive')) die('skip wecomarchive not loaded');
if (!is_file(getenv('WECOM_MOCK_SDK') ?: '/usr/local/lib/libMockWeComSdk.so')) die('skip mock SDK not found');
