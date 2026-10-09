package org.difft.android.smp;

public interface HttpCallback {
    void onResponse(HttpResponse response);

    void onError(int result, String errorName, String errorMessage);
}
