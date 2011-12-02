/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is B2G.
 *
 * The Initial Developer of the Original Code is
 * Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2011
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

const Cc = Components.classes;
const Ci = Components.interfaces;
const Cu = Components.utils;
const CC = Components.Constructor;

Cu.import('resource://gre/modules/Services.jsm');

const LocalFile = CC('@mozilla.org/file/local;1',
                     'nsILocalFile',
                     'initWithPath');
var shell = {
  get home() {
    delete this.home;
    return this.home = document.getElementById('homescreen');
  },

  get homeSrc() {
    try {
      let homeSrc = Cc['@mozilla.org/process/environment;1']
                      .getService(Ci.nsIEnvironment)
                      .get('B2G_HOMESCREEN');
      if (homeSrc)
        return homeSrc;
    } catch (e) {}

    let urls = Services.prefs.getCharPref('browser.homescreenURL').split(',');
    for (let i = 0; i < urls.length; i++) {
      let url = urls[i];
      if (url.substring(0, 7) != 'file://')
        return url;

      let file = new LocalFile(url.substring(7, url.length));
      if (file.exists())
        return url;
    }
    return null;
  },

  startMarionetteServer: function shell_startMarionetteServer() {
    dump("starting Marionette....");
    try {
      Cu.import("resource:///modules/marionette-logger.jsm");
      Cu.import("resource:///modules/dbg-server.jsm");
      DebuggerServer.addActors("resource:///modules/marionette-actors.js");
      DebuggerServer.initTransport();
      DebuggerServer.openListener(2828, true);
      MarionetteLogger.write('opened listener on port 2828');
    }
    catch(e) {
      dump("exception: " + e.name + ", " + e.message);
    }
  },

  start: function shell_init() {
    window.controllers.appendController(this);
    window.addEventListener('keypress', this);
    this.home.addEventListener('load', this, true);

    let ioService = Cc['@mozilla.org/network/io-service;1']
                      .getService(Ci.nsIIOService2);
    ioService.offline = false;

    let browser = this.home;
    browser.homePage = this.homeSrc;
    browser.goHome();

    // XXX: this should be gated on a pref, and might need to be placed
    // somewhere else
    this.startMarionetteServer();
  },

  stop: function shell_stop() {
    window.controllers.removeController(this);
    window.removeEventListener('keypress', this);
  },

  supportsCommand: function shell_supportsCommand(cmd) {
    let isSupported = false;
    switch (cmd) {
      case 'cmd_close':
        isSupported = true;
        break;
      default:
        isSupported = false;
        break;
    }
    return isSupported;
  },

  isCommandEnabled: function shell_isCommandEnabled(cmd) {
    return true;
  },

  doCommand: function shell_doCommand(cmd) {
    switch (cmd) {
      case 'cmd_close':
        this.sendEvent(this.home.contentWindow, 'appclose');
        break;
    }
  },

  handleEvent: function shell_handleEvent(evt) {
    switch (evt.type) {
      case 'keypress':
        switch (evt.keyCode) {
          case evt.DOM_VK_HOME:
            this.sendEvent(this.home.contentWindow, 'home');
            break;
          case evt.DOM_VK_ESCAPE:
            if (evt.getPreventDefault())
              return;
            this.doCommand('cmd_close');
            break;
        }
        break;
      case 'load':
        this.home.removeEventListener('load', this, true);
        this.sendEvent(window, 'ContentStart');
        break;
    }
  },
  sendEvent: function shell_sendEvent(content, type, details) {
    let event = content.document.createEvent('CustomEvent');
    event.initCustomEvent(type, true, true, details ? details : {});
    content.dispatchEvent(event);
  }
};

(function VirtualKeyboardManager() {
  let activeElement = null;
  let isKeyboardOpened = false;

  let constructor = {
    handleEvent: function vkm_handleEvent(evt) {
      let contentWindow = shell.home.contentWindow.wrappedJSObject;

      switch (evt.type) {
        case 'ContentStart':
          contentWindow.navigator.mozKeyboard = new MozKeyboard();
          break;
        case 'keypress':
          if (evt.keyCode != evt.DOM_VK_ESCAPE || !isKeyboardOpened)
            return;

          shell.sendEvent(contentWindow, 'hideime');
          isKeyboardOpened = false;

          evt.preventDefault();
          evt.stopPropagation();
          break;
        case 'mousedown':
          if (evt.target != activeElement || isKeyboardOpened)
            return;

          let type = activeElement.type;
          shell.sendEvent(contentWindow, 'showime', { type: type });
          isKeyboardOpened = true;
          break;
      }
    },
    observe: function vkm_observe(subject, topic, data) {
      let contentWindow = shell.home.contentWindow;

      let shouldOpen = parseInt(data);
      if (shouldOpen && !isKeyboardOpened) {
        activeElement = Cc['@mozilla.org/focus-manager;1']
                          .getService(Ci.nsIFocusManager)
                          .focusedElement;
        if (!activeElement)
          return;

        let type = activeElement.type;
        shell.sendEvent(contentWindow, 'showime', { type: type });
      } else if (!shouldOpen && isKeyboardOpened) {
        shell.sendEvent(contentWindow, 'hideime');
      }
      isKeyboardOpened = shouldOpen;
    }
  };

  Services.obs.addObserver(constructor, "ime-enabled-state-changed", false);
  ['ContentStart', 'keypress', 'mousedown'].forEach(function vkm_events(type) {
    window.addEventListener(type, constructor, true);
  });
})();


function MozKeyboard() {
}

MozKeyboard.prototype = {
  sendKey: function mozKeyboardSendKey(keyCode) {
    var utils = window.QueryInterface(Ci.nsIInterfaceRequestor)
                      .getInterface(Ci.nsIDOMWindowUtils);
    ['keydown', 'keypress', 'keyup'].forEach(function sendKeyEvents(type) {
      utils.sendKeyEvent(type, keyCode, keyCode, null);
    });
  }
};

