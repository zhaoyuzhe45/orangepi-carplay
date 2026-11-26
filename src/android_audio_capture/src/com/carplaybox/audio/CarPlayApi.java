package com.carplaybox.audio;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

final class CarPlayApi {
    static final int DEFAULT_PORT = 49152;
    private volatile int port;

    CarPlayApi(int port) {
        setPort(port);
    }

    void setPort(int value) {
        if (value >= 1024 && value <= 65535) port = value;
    }

    int getPort() { return port; }

    Result get(String path) throws IOException {
        return request("GET", path, null);
    }

    Result putJson(String path, String body) throws IOException {
        return request("PUT", path, body);
    }

    Result post(String path) throws IOException {
        return request("POST", path, "");
    }

    private Result request(String method, String path, String body) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(
                "http://127.0.0.1:" + port + path).openConnection();
        connection.setRequestMethod(method);
        connection.setConnectTimeout(2500);
        connection.setReadTimeout(5000);
        connection.setUseCaches(false);
        connection.setRequestProperty("Accept", "application/json");
        if (body != null) {
            byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
            connection.setDoOutput(true);
            connection.setRequestProperty("Content-Type", "application/json");
            connection.setFixedLengthStreamingMode(bytes.length);
            try (OutputStream output = connection.getOutputStream()) {
                output.write(bytes);
            }
        }
        int code = connection.getResponseCode();
        InputStream stream = code >= 200 && code < 300
                ? connection.getInputStream() : connection.getErrorStream();
        String text = readAll(stream);
        connection.disconnect();
        return new Result(code, text);
    }

    private static String readAll(InputStream stream) throws IOException {
        if (stream == null) return "";
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = stream.read(buffer)) >= 0) output.write(buffer, 0, count);
        try (InputStream closeable = stream) {
            return new String(output.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    static final class Result {
        final int code;
        final String body;
        Result(int code, String body) {
            this.code = code;
            this.body = body;
        }
    }
}