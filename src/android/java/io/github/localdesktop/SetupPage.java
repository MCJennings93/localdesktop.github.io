package io.github.localdesktop;

import android.app.Activity;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.PopupWindow;

/**
 * The setup screen: a WebView in a popup over the whole activity. Views belong to the main thread,
 * which also delivers configuration changes (rotation, folding, font scale) to every window of the
 * activity, so the popup is built and updated there; built on another thread, the first
 * configuration change crashed the app. One popup per activity: showing it again (every resume)
 * only loads a different page into it.
 */
public final class SetupPage {
    private static Activity owner;
    private static PopupWindow popup;
    private static WebView webView;
    private static String shownUrl;

    private SetupPage() {}

    public static void show(final Activity activity, final String url) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                showOnUiThread(activity, url);
            }
        });
    }

    private static void showOnUiThread(final Activity activity, final String url) {
        if (owner == activity && popup != null && popup.isShowing()) {
            if (!url.equals(shownUrl)) {
                webView.loadUrl(url);
                shownUrl = url;
            }
            return;
        }
        if (popup != null) {
            popup.dismiss();
            popup = null;
        }
        final View parent = activity.getWindow().getDecorView();
        // A popup needs the activity's window, which may not be attached yet early in a resume.
        if (parent.getWindowToken() == null) {
            parent.post(new Runnable() {
                @Override
                public void run() {
                    showOnUiThread(activity, url);
                }
            });
            return;
        }
        webView = new WebView(activity);
        webView.getSettings().setJavaScriptEnabled(true);
        // Links stay in the WebView instead of opening a browser.
        webView.setWebViewClient(new WebViewClient());
        webView.loadUrl(url);
        popup = new PopupWindow(webView, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        popup.showAtLocation(parent, Gravity.CENTER, 0, 0);
        owner = activity;
        shownUrl = url;
    }
}
