/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Util.h"

#include "nsCOMPtr.h"
#include "nsXBLPrototypeHandler.h"
#include "nsXBLPrototypeBinding.h"
#include "nsContentUtils.h"
#include "nsIContent.h"
#include "nsIAtom.h"
#include "nsIDOMKeyEvent.h"
#include "nsIDOMMouseEvent.h"
#include "nsINameSpaceManager.h"
#include "nsIScriptContext.h"
#include "nsIDocument.h"
#include "nsIDOMDocument.h"
#include "nsIJSEventListener.h"
#include "nsIController.h"
#include "nsIControllers.h"
#include "nsIDOMXULElement.h"
#include "nsIURI.h"
#include "nsIDOMHTMLTextAreaElement.h"
#include "nsIDOMHTMLInputElement.h"
#include "nsFocusManager.h"
#include "nsEventListenerManager.h"
#include "nsIDOMEventTarget.h"
#include "nsIDOMEventListener.h"
#include "nsIPrivateDOMEvent.h"
#include "nsIDOMNSEvent.h"
#include "nsPIDOMWindow.h"
#include "nsPIWindowRoot.h"
#include "nsIDOMWindow.h"
#include "nsIServiceManager.h"
#include "nsIScriptError.h"
#include "nsXPIDLString.h"
#include "nsReadableUtils.h"
#include "nsGkAtoms.h"
#include "nsGUIEvent.h"
#include "nsIXPConnect.h"
#include "nsIDOMScriptObjectFactory.h"
#include "nsDOMCID.h"
#include "nsUnicharUtils.h"
#include "nsCRT.h"
#include "nsXBLEventHandler.h"
#include "nsXBLSerialize.h"
#include "nsEventDispatcher.h"
#include "mozilla/Preferences.h"

using namespace mozilla;

static NS_DEFINE_CID(kDOMScriptObjectFactoryCID,
                     NS_DOM_SCRIPT_OBJECT_FACTORY_CID);

PRUint32 nsXBLPrototypeHandler::gRefCnt = 0;

PRInt32 nsXBLPrototypeHandler::kMenuAccessKey = -1;
PRInt32 nsXBLPrototypeHandler::kAccelKey = -1;

const PRInt32 nsXBLPrototypeHandler::cShift = (1<<0);
const PRInt32 nsXBLPrototypeHandler::cAlt = (1<<1);
const PRInt32 nsXBLPrototypeHandler::cControl = (1<<2);
const PRInt32 nsXBLPrototypeHandler::cMeta = (1<<3);

const PRInt32 nsXBLPrototypeHandler::cShiftMask = (1<<4);
const PRInt32 nsXBLPrototypeHandler::cAltMask = (1<<5);
const PRInt32 nsXBLPrototypeHandler::cControlMask = (1<<6);
const PRInt32 nsXBLPrototypeHandler::cMetaMask = (1<<7);

const PRInt32 nsXBLPrototypeHandler::cAllModifiers = cShiftMask | cAltMask | cControlMask | cMetaMask;

nsXBLPrototypeHandler::nsXBLPrototypeHandler(const PRUnichar* aEvent,
                                             const PRUnichar* aPhase,
                                             const PRUnichar* aAction,
                                             const PRUnichar* aCommand,
                                             const PRUnichar* aKeyCode,
                                             const PRUnichar* aCharCode,
                                             const PRUnichar* aModifiers,
                                             const PRUnichar* aButton,
                                             const PRUnichar* aClickCount,
                                             const PRUnichar* aGroup,
                                             const PRUnichar* aPreventDefault,
                                             const PRUnichar* aAllowUntrusted,
                                             nsXBLPrototypeBinding* aBinding,
                                             PRUint32 aLineNumber)
  : mHandlerText(nsnull),
    mLineNumber(aLineNumber),
    mNextHandler(nsnull),
    mPrototypeBinding(aBinding)
{
  Init();

  ConstructPrototype(nsnull, aEvent, aPhase, aAction, aCommand, aKeyCode,
                     aCharCode, aModifiers, aButton, aClickCount,
                     aGroup, aPreventDefault, aAllowUntrusted);
}

nsXBLPrototypeHandler::nsXBLPrototypeHandler(nsIContent* aHandlerElement)
  : mHandlerElement(nsnull),
    mLineNumber(0),
    mNextHandler(nsnull),
    mPrototypeBinding(nsnull)
{
  Init();

  // Make sure our prototype is initialized.
  ConstructPrototype(aHandlerElement);
}

nsXBLPrototypeHandler::nsXBLPrototypeHandler(nsXBLPrototypeBinding* aBinding)
  : mHandlerText(nsnull),
    mLineNumber(nsnull),
    mNextHandler(nsnull),
    mPrototypeBinding(aBinding)
{
  Init();
}

nsXBLPrototypeHandler::~nsXBLPrototypeHandler()
{
  --gRefCnt;
  if (mType & NS_HANDLER_TYPE_XUL) {
    NS_IF_RELEASE(mHandlerElement);
  } else if (mHandlerText) {
    nsMemory::Free(mHandlerText);
  }

  // We own the next handler in the chain, so delete it now.
  NS_CONTENT_DELETE_LIST_MEMBER(nsXBLPrototypeHandler, this, mNextHandler);
}

already_AddRefed<nsIContent>
nsXBLPrototypeHandler::GetHandlerElement()
{
  if (mType & NS_HANDLER_TYPE_XUL) {
    nsCOMPtr<nsIContent> element = do_QueryReferent(mHandlerElement);
    nsIContent* el = nsnull;
    element.swap(el);
    return el;
  }

  return nsnull;
}

void
nsXBLPrototypeHandler::AppendHandlerText(const nsAString& aText) 
{
  if (mHandlerText) {
    // Append our text to the existing text.
    PRUnichar* temp = mHandlerText;
    mHandlerText = ToNewUnicode(nsDependentString(temp) + aText);
    nsMemory::Free(temp);
  }
  else {
    mHandlerText = ToNewUnicode(aText);
  }
}

/////////////////////////////////////////////////////////////////////////////
// Get the menu access key from prefs.
// XXX Eventually pick up using CSS3 key-equivalent property or somesuch
void
nsXBLPrototypeHandler::InitAccessKeys()
{
  if (kAccelKey >= 0 && kMenuAccessKey >= 0)
    return;

  // Compiled-in defaults, in case we can't get the pref --
  // mac doesn't have menu shortcuts, other platforms use alt.
#ifdef XP_MACOSX
  kMenuAccessKey = 0;
  kAccelKey = nsIDOMKeyEvent::DOM_VK_META;
#else
  kMenuAccessKey = nsIDOMKeyEvent::DOM_VK_ALT;
  kAccelKey = nsIDOMKeyEvent::DOM_VK_CONTROL;
#endif

  // Get the menu access key value from prefs, overriding the default:
  kMenuAccessKey =
    Preferences::GetInt("ui.key.menuAccessKey", kMenuAccessKey);
  kAccelKey = Preferences::GetInt("ui.key.accelKey", kAccelKey);
}

nsresult
nsXBLPrototypeHandler::ExecuteHandler(nsIDOMEventTarget* aTarget,
                                      nsIDOMEvent* aEvent)
{
  nsresult rv = NS_ERROR_FAILURE;

  // Prevent default action?
  if (mType & NS_HANDLER_TYPE_PREVENTDEFAULT) {
    aEvent->PreventDefault();
    // If we prevent default, then it's okay for
    // mHandlerElement and mHandlerText to be null
    rv = NS_OK;
  }

  if (!mHandlerElement) // This works for both types of handlers.  In both cases, the union's var should be defined.
    return rv;

  // See if our event receiver is a content node (and not us).
  bool isXULKey = !!(mType & NS_HANDLER_TYPE_XUL);
  bool isXBLCommand = !!(mType & NS_HANDLER_TYPE_XBL_COMMAND);
  NS_ASSERTION(!(isXULKey && isXBLCommand),
               "can't be both a key and xbl command handler");

  // XUL handlers and commands shouldn't be triggered by non-trusted
  // events.
  if (isXULKey || isXBLCommand) {
    nsCOMPtr<nsIDOMNSEvent> domNSEvent = do_QueryInterface(aEvent);
    bool trustedEvent = false;
    if (domNSEvent) {
      domNSEvent->GetIsTrusted(&trustedEvent);
    }

    if (!trustedEvent)
      return NS_OK;
  }
    
  if (isXBLCommand) {
    return DispatchXBLCommand(aTarget, aEvent);
  }

  // If we're executing on a XUL key element, just dispatch a command
  // event at the element.  It will take care of retargeting it to its
  // command element, if applicable, and executing the event handler.
  if (isXULKey) {
    return DispatchXULKeyCommand(aEvent);
  }

  // Look for a compiled handler on the element. 
  // Should be compiled and bound with "on" in front of the name.
  nsCOMPtr<nsIAtom> onEventAtom = do_GetAtom(NS_LITERAL_STRING("onxbl") +
                                             nsDependentAtomString(mEventName));

  // Compile the handler and bind it to the element.
  nsCOMPtr<nsIScriptGlobalObject> boundGlobal;
  nsCOMPtr<nsPIWindowRoot> winRoot(do_QueryInterface(aTarget));
  nsCOMPtr<nsPIDOMWindow> window;

  if (winRoot) {
    window = winRoot->GetWindow();
  }

  if (window) {
    window = window->GetCurrentInnerWindow();
    NS_ENSURE_TRUE(window, NS_ERROR_UNEXPECTED);

    boundGlobal = do_QueryInterface(window->GetPrivateRoot());
  }
  else boundGlobal = do_QueryInterface(aTarget);

  if (!boundGlobal) {
    nsCOMPtr<nsIDocument> boundDocument(do_QueryInterface(aTarget));
    if (!boundDocument) {
      // We must be an element.
      nsCOMPtr<nsIContent> content(do_QueryInterface(aTarget));
      if (!content)
        return NS_OK;
      boundDocument = content->OwnerDoc();
    }

    boundGlobal = boundDocument->GetScopeObject();
  }

  if (!boundGlobal)
    return NS_OK;

  nsIScriptContext *boundContext = boundGlobal->GetScriptContext();
  if (!boundContext)
    return NS_OK;

  nsScriptObjectHolder<JSObject> handler(boundContext);
  nsISupports *scriptTarget;

  if (winRoot) {
    scriptTarget = boundGlobal;
  } else {
    scriptTarget = aTarget;
  }

  rv = EnsureEventHandler(boundGlobal, boundContext, onEventAtom, handler);
  NS_ENSURE_SUCCESS(rv, rv);

  // Bind it to the bound element
  JSObject* scope = boundGlobal->GetGlobalJSObject();
  nsScriptObjectHolder<JSObject> boundHandler(boundContext);
  rv = boundContext->BindCompiledEventHandler(scriptTarget, scope,
                                              handler.get(), boundHandler);
  NS_ENSURE_SUCCESS(rv, rv);

  // Execute it.
  nsCOMPtr<nsIJSEventListener> eventListener;
  rv = NS_NewJSEventListener(boundContext, scope,
                             scriptTarget, onEventAtom,
                             boundHandler.get(),
                             getter_AddRefs(eventListener));
  NS_ENSURE_SUCCESS(rv, rv);

  // Handle the event.
  eventListener->HandleEvent(aEvent);
  eventListener->Disconnect();
  return NS_OK;
}

nsresult
nsXBLPrototypeHandler::EnsureEventHandler(nsIScriptGlobalObject* aGlobal,
                                          nsIScriptContext *aBoundContext,
                                          nsIAtom *aName,
                                          nsScriptObjectHolder<JSObject>& aHandler)
{
  // Check to see if we've already compiled this
  nsCOMPtr<nsPIDOMWindow> pWindow = do_QueryInterface(aGlobal);
  if (pWindow) {
    JSObject* cachedHandler = pWindow->GetCachedXBLPrototypeHandler(this);
    if (cachedHandler) {
      aHandler.set(cachedHandler);
      return aHandler ? NS_OK : NS_ERROR_FAILURE;
    }
  }

  // Ensure that we have something to compile
  nsDependentString handlerText(mHandlerText);
  if (handlerText.IsEmpty())
    return NS_ERROR_FAILURE;

  nsCAutoString bindingURI;
  mPrototypeBinding->DocURI()->GetSpec(bindingURI);

  PRUint32 argCount;
  const char **argNames;
  nsContentUtils::GetEventArgNames(kNameSpaceID_XBL, aName, &argCount,
                                   &argNames);
  nsresult rv = aBoundContext->CompileEventHandler(aName, argCount, argNames,
                                                   handlerText,
                                                   bindingURI.get(), 
                                                   mLineNumber,
                                                   JSVERSION_LATEST, aHandler);
  NS_ENSURE_SUCCESS(rv, rv);

  if (pWindow) {
    pWindow->CacheXBLPrototypeHandler(this, aHandler);
  }

  return NS_OK;
}

nsresult
nsXBLPrototypeHandler::DispatchXBLCommand(nsIDOMEventTarget* aTarget, nsIDOMEvent* aEvent)
{
  // This is a special-case optimization to make command handling fast.
  // It isn't really a part of XBL, but it helps speed things up.

  // See if preventDefault has been set.  If so, don't execute.
  bool preventDefault = false;
  nsCOMPtr<nsIDOMNSEvent> domNSEvent = do_QueryInterface(aEvent);
  if (domNSEvent) {
    domNSEvent->GetPreventDefault(&preventDefault);
  }

  if (preventDefault)
    return NS_OK;

  nsCOMPtr<nsIPrivateDOMEvent> privateEvent = do_QueryInterface(aEvent);
  if (privateEvent) {
    bool dispatchStopped = privateEvent->IsDispatchStopped();
    if (dispatchStopped)
      return NS_OK;
  }

  // Instead of executing JS, let's get the controller for the bound
  // element and call doCommand on it.
  nsCOMPtr<nsIController> controller;

  nsCOMPtr<nsPIDOMWindow> privateWindow;
  nsCOMPtr<nsPIWindowRoot> windowRoot(do_QueryInterface(aTarget));
  if (windowRoot) {
    privateWindow = windowRoot->GetWindow();
  }
  else {
    privateWindow = do_QueryInterface(aTarget);
    if (!privateWindow) {
      nsCOMPtr<nsIContent> elt(do_QueryInterface(aTarget));
      nsCOMPtr<nsIDocument> doc;
      // XXXbz sXBL/XBL2 issue -- this should be the "scope doc" or
      // something... whatever we use when wrapping DOM nodes
      // normally.  It's not clear that the owner doc is the right
      // thing.
      if (elt)
        doc = elt->OwnerDoc();

      if (!doc)
        doc = do_QueryInterface(aTarget);

      if (!doc)
        return NS_ERROR_FAILURE;

      privateWindow = do_QueryInterface(doc->GetScriptGlobalObject());
      if (!privateWindow)
        return NS_ERROR_FAILURE;
    }

    windowRoot = privateWindow->GetTopWindowRoot();
  }

  NS_LossyConvertUTF16toASCII command(mHandlerText);
  if (windowRoot)
    windowRoot->GetControllerForCommand(command.get(), getter_AddRefs(controller));
  else
    controller = GetController(aTarget); // We're attached to the receiver possibly.

  if (mEventName == nsGkAtoms::keypress &&
      mDetail == nsIDOMKeyEvent::DOM_VK_SPACE &&
      mMisc == 1) {
    // get the focused element so that we can pageDown only at
    // certain times.

    nsCOMPtr<nsPIDOMWindow> windowToCheck;
    if (windowRoot)
      windowToCheck = windowRoot->GetWindow();
    else
      windowToCheck = privateWindow->GetPrivateRoot();

    nsCOMPtr<nsIContent> focusedContent;
    if (windowToCheck) {
      nsCOMPtr<nsPIDOMWindow> focusedWindow;
      focusedContent =
        nsFocusManager::GetFocusedDescendant(windowToCheck, true, getter_AddRefs(focusedWindow));
    }

    bool isLink = false;
    nsIContent *content = focusedContent;

    // if the focused element is a link then we do want space to 
    // scroll down. The focused element may be an element in a link,
    // we need to check the parent node too. Only do this check if an
    // element is focused and has a parent.
    if (focusedContent && focusedContent->GetParent()) {
      while (content) {
        if (content->Tag() == nsGkAtoms::a && content->IsHTML()) {
          isLink = true;
          break;
        }

        if (content->HasAttr(kNameSpaceID_XLink, nsGkAtoms::type)) {
          isLink = content->AttrValueIs(kNameSpaceID_XLink, nsGkAtoms::type,
                                        nsGkAtoms::simple, eCaseMatters);

          if (isLink) {
            break;
          }
        }

        content = content->GetParent();
      }

      if (!isLink)
        return NS_OK;
    }
  }

  // We are the default action for this command.
  // Stop any other default action from executing.
  aEvent->PreventDefault();
  
  if (controller)
    controller->DoCommand(command.get());

  return NS_OK;
}

nsresult
nsXBLPrototypeHandler::DispatchXULKeyCommand(nsIDOMEvent* aEvent)
{
  nsCOMPtr<nsIContent> handlerElement = GetHandlerElement();
  NS_ENSURE_STATE(handlerElement);
  if (handlerElement->AttrValueIs(kNameSpaceID_None,
                                  nsGkAtoms::disabled,
                                  nsGkAtoms::_true,
                                  eCaseMatters)) {
    // Don't dispatch command events for disabled keys.
    return NS_OK;
  }

  aEvent->PreventDefault();

  // Copy the modifiers from the key event.
  nsCOMPtr<nsIDOMKeyEvent> keyEvent = do_QueryInterface(aEvent);
  if (!keyEvent) {
    NS_ERROR("Trying to execute a key handler for a non-key event!");
    return NS_ERROR_FAILURE;
  }

  bool isAlt = false;
  bool isControl = false;
  bool isShift = false;
  bool isMeta = false;
  keyEvent->GetAltKey(&isAlt);
  keyEvent->GetCtrlKey(&isControl);
  keyEvent->GetShiftKey(&isShift);
  keyEvent->GetMetaKey(&isMeta);

  nsContentUtils::DispatchXULCommand(handlerElement, true,
                                     nsnull, nsnull,
                                     isControl, isAlt, isShift, isMeta);
  return NS_OK;
}

already_AddRefed<nsIAtom>
nsXBLPrototypeHandler::GetEventName()
{
  nsIAtom* eventName = mEventName;
  NS_IF_ADDREF(eventName);
  return eventName;
}

already_AddRefed<nsIController>
nsXBLPrototypeHandler::GetController(nsIDOMEventTarget* aTarget)
{
  // XXX Fix this so there's a generic interface that describes controllers, 
  // This code should have no special knowledge of what objects might have controllers.
  nsCOMPtr<nsIControllers> controllers;

  nsCOMPtr<nsIDOMXULElement> xulElement(do_QueryInterface(aTarget));
  if (xulElement)
    xulElement->GetControllers(getter_AddRefs(controllers));

  if (!controllers) {
    nsCOMPtr<nsIDOMHTMLTextAreaElement> htmlTextArea(do_QueryInterface(aTarget));
    if (htmlTextArea)
      htmlTextArea->GetControllers(getter_AddRefs(controllers));
  }

  if (!controllers) {
    nsCOMPtr<nsIDOMHTMLInputElement> htmlInputElement(do_QueryInterface(aTarget));
    if (htmlInputElement)
      htmlInputElement->GetControllers(getter_AddRefs(controllers));
  }

  if (!controllers) {
    nsCOMPtr<nsIDOMWindow> domWindow(do_QueryInterface(aTarget));
    if (domWindow)
      domWindow->GetControllers(getter_AddRefs(controllers));
  }

  // Return the first controller.
  // XXX This code should be checking the command name and using supportscommand and
  // iscommandenabled.
  nsIController* controller;
  if (controllers) {
    controllers->GetControllerAt(0, &controller);  // return reference
  }
  else controller = nsnull;

  return controller;
}

bool
nsXBLPrototypeHandler::KeyEventMatched(nsIDOMKeyEvent* aKeyEvent,
                                       PRUint32 aCharCode,
                                       bool aIgnoreShiftKey)
{
  if (mDetail != -1) {
    // Get the keycode or charcode of the key event.
    PRUint32 code;

    if (mMisc) {
      if (aCharCode)
        code = aCharCode;
      else
        aKeyEvent->GetCharCode(&code);
      if (IS_IN_BMP(code))
        code = ToLowerCase(PRUnichar(code));
    }
    else
      aKeyEvent->GetKeyCode(&code);

    if (code != PRUint32(mDetail))
      return false;
  }

  return ModifiersMatchMask(aKeyEvent, aIgnoreShiftKey);
}

bool
nsXBLPrototypeHandler::MouseEventMatched(nsIDOMMouseEvent* aMouseEvent)
{
  if (mDetail == -1 && mMisc == 0 && (mKeyMask & cAllModifiers) == 0)
    return true; // No filters set up. It's generic.

  PRUint16 button;
  aMouseEvent->GetButton(&button);
  if (mDetail != -1 && (button != mDetail))
    return false;

  PRInt32 clickcount;
  aMouseEvent->GetDetail(&clickcount);
  if (mMisc != 0 && (clickcount != mMisc))
    return false;

  return ModifiersMatchMask(aMouseEvent);
}

struct keyCodeData {
  const char* str;
  size_t strlength;
  PRUint32 keycode;
};

// All of these must be uppercase, since the function below does
// case-insensitive comparison by converting to uppercase.
// XXX: be sure to check this periodically for new symbol additions!
static const keyCodeData gKeyCodes[] = {

#define NS_DEFINE_VK(aDOMKeyName, aDOMKeyCode) \
  { #aDOMKeyName, sizeof(#aDOMKeyName) - 1, aDOMKeyCode }
#include "nsVKList.h"
#undef NS_DEFINE_VK
};

PRInt32 nsXBLPrototypeHandler::GetMatchingKeyCode(const nsAString& aKeyName)
{
  nsCAutoString keyName;
  keyName.AssignWithConversion(aKeyName);
  ToUpperCase(keyName); // We want case-insensitive comparison with data
                        // stored as uppercase.

  PRUint32 keyNameLength = keyName.Length();
  const char* keyNameStr = keyName.get();
  for (PRUint16 i = 0; i < (sizeof(gKeyCodes) / sizeof(gKeyCodes[0])); ++i)
    if (keyNameLength == gKeyCodes[i].strlength &&
        !nsCRT::strcmp(gKeyCodes[i].str, keyNameStr))
      return gKeyCodes[i].keycode;

  return 0;
}

PRInt32 nsXBLPrototypeHandler::KeyToMask(PRInt32 key)
{
  switch (key)
  {
    case nsIDOMKeyEvent::DOM_VK_META:
      return cMeta | cMetaMask;
      break;

    case nsIDOMKeyEvent::DOM_VK_ALT:
      return cAlt | cAltMask;
      break;

    case nsIDOMKeyEvent::DOM_VK_CONTROL:
    default:
      return cControl | cControlMask;
  }
  return cControl | cControlMask;  // for warning avoidance
}

void
nsXBLPrototypeHandler::GetEventType(nsAString& aEvent)
{
  nsCOMPtr<nsIContent> handlerElement = GetHandlerElement();
  if (!handlerElement) {
    aEvent.Truncate();
    return;
  }
  handlerElement->GetAttr(kNameSpaceID_None, nsGkAtoms::event, aEvent);
  
  if (aEvent.IsEmpty() && (mType & NS_HANDLER_TYPE_XUL))
    // If no type is specified for a XUL <key> element, let's assume that we're "keypress".
    aEvent.AssignLiteral("keypress");
}

void
nsXBLPrototypeHandler::ConstructPrototype(nsIContent* aKeyElement, 
                                          const PRUnichar* aEvent,
                                          const PRUnichar* aPhase,
                                          const PRUnichar* aAction,
                                          const PRUnichar* aCommand,
                                          const PRUnichar* aKeyCode,
                                          const PRUnichar* aCharCode,
                                          const PRUnichar* aModifiers,
                                          const PRUnichar* aButton,
                                          const PRUnichar* aClickCount,
                                          const PRUnichar* aGroup,
                                          const PRUnichar* aPreventDefault,
                                          const PRUnichar* aAllowUntrusted)
{
  mType = 0;

  if (aKeyElement) {
    mType |= NS_HANDLER_TYPE_XUL;
    nsCOMPtr<nsIWeakReference> weak = do_GetWeakReference(aKeyElement);
    if (!weak) {
      return;
    }
    weak.swap(mHandlerElement);
  }
  else {
    mType |= aCommand ? NS_HANDLER_TYPE_XBL_COMMAND : NS_HANDLER_TYPE_XBL_JS;
    mHandlerText = nsnull;
  }

  mDetail = -1;
  mMisc = 0;
  mKeyMask = 0;
  mPhase = NS_PHASE_BUBBLING;

  if (aAction)
    mHandlerText = ToNewUnicode(nsDependentString(aAction));
  else if (aCommand)
    mHandlerText = ToNewUnicode(nsDependentString(aCommand));

  nsAutoString event(aEvent);
  if (event.IsEmpty()) {
    if (mType & NS_HANDLER_TYPE_XUL)
      GetEventType(event);
    if (event.IsEmpty())
      return;
  }

  mEventName = do_GetAtom(event);

  if (aPhase) {
    const nsDependentString phase(aPhase);
    if (phase.EqualsLiteral("capturing"))
      mPhase = NS_PHASE_CAPTURING;
    else if (phase.EqualsLiteral("target"))
      mPhase = NS_PHASE_TARGET;
  }

  // Button and clickcount apply only to XBL handlers and don't apply to XUL key
  // handlers.  
  if (aButton && *aButton)
    mDetail = *aButton - '0';

  if (aClickCount && *aClickCount)
    mMisc = *aClickCount - '0';

  // Modifiers are supported by both types of handlers (XUL and XBL).  
  nsAutoString modifiers(aModifiers);
  if (mType & NS_HANDLER_TYPE_XUL)
    aKeyElement->GetAttr(kNameSpaceID_None, nsGkAtoms::modifiers, modifiers);
  
  if (!modifiers.IsEmpty()) {
    mKeyMask = cAllModifiers;
    char* str = ToNewCString(modifiers);
    char* newStr;
    char* token = nsCRT::strtok( str, ", \t", &newStr );
    while( token != NULL ) {
      if (PL_strcmp(token, "shift") == 0)
        mKeyMask |= cShift | cShiftMask;
      else if (PL_strcmp(token, "alt") == 0)
        mKeyMask |= cAlt | cAltMask;
      else if (PL_strcmp(token, "meta") == 0)
        mKeyMask |= cMeta | cMetaMask;
      else if (PL_strcmp(token, "control") == 0)
        mKeyMask |= cControl | cControlMask;
      else if (PL_strcmp(token, "accel") == 0)
        mKeyMask |= KeyToMask(kAccelKey);
      else if (PL_strcmp(token, "access") == 0)
        mKeyMask |= KeyToMask(kMenuAccessKey);
      else if (PL_strcmp(token, "any") == 0)
        mKeyMask &= ~(mKeyMask << 4);
    
      token = nsCRT::strtok( newStr, ", \t", &newStr );
    }

    nsMemory::Free(str);
  }

  nsAutoString key(aCharCode);
  if (key.IsEmpty()) {
    if (mType & NS_HANDLER_TYPE_XUL) {
      aKeyElement->GetAttr(kNameSpaceID_None, nsGkAtoms::key, key);
      if (key.IsEmpty()) 
        aKeyElement->GetAttr(kNameSpaceID_None, nsGkAtoms::charcode, key);
    }
  }

  if (!key.IsEmpty()) {
    if (mKeyMask == 0)
      mKeyMask = cAllModifiers;
    nsContentUtils::ASCIIToLower(key);

    // We have a charcode.
    mMisc = 1;
    mDetail = key[0];
    const PRUint8 GTK2Modifiers = cShift | cControl | cShiftMask | cControlMask;
    if ((mKeyMask & GTK2Modifiers) == GTK2Modifiers &&
        modifiers.First() != PRUnichar(',') && mDetail == 'u')
      ReportKeyConflict(key.get(), modifiers.get(), aKeyElement, "GTK2Conflict");
    const PRUint8 WinModifiers = cControl | cAlt | cControlMask | cAltMask;
    if ((mKeyMask & WinModifiers) == WinModifiers &&
        modifiers.First() != PRUnichar(',') && ('a' <= mDetail && mDetail <= 'z'))
      ReportKeyConflict(key.get(), modifiers.get(), aKeyElement, "WinConflict");
  }
  else {
    key.Assign(aKeyCode);
    if (mType & NS_HANDLER_TYPE_XUL)
      aKeyElement->GetAttr(kNameSpaceID_None, nsGkAtoms::keycode, key);
    
    if (!key.IsEmpty()) {
      if (mKeyMask == 0)
        mKeyMask = cAllModifiers;
      mDetail = GetMatchingKeyCode(key);
    }
  }

  if (aGroup && nsDependentString(aGroup).EqualsLiteral("system"))
    mType |= NS_HANDLER_TYPE_SYSTEM;

  if (aPreventDefault &&
      nsDependentString(aPreventDefault).EqualsLiteral("true"))
    mType |= NS_HANDLER_TYPE_PREVENTDEFAULT;

  if (aAllowUntrusted) {
    mType |= NS_HANDLER_HAS_ALLOW_UNTRUSTED_ATTR;
    if (nsDependentString(aAllowUntrusted).EqualsLiteral("true")) {
      mType |= NS_HANDLER_ALLOW_UNTRUSTED;
    } else {
      mType &= ~NS_HANDLER_ALLOW_UNTRUSTED;
    }
  }
}

void
nsXBLPrototypeHandler::ReportKeyConflict(const PRUnichar* aKey, const PRUnichar* aModifiers, nsIContent* aKeyElement, const char *aMessageName)
{
  nsCOMPtr<nsIDocument> doc;
  if (mPrototypeBinding) {
    nsXBLDocumentInfo* docInfo = mPrototypeBinding->XBLDocumentInfo();
    if (docInfo) {
      doc = docInfo->GetDocument();
    }
  } else if (aKeyElement) {
    doc = aKeyElement->OwnerDoc();
  }

  const PRUnichar* params[] = { aKey, aModifiers };
  nsContentUtils::ReportToConsole(nsIScriptError::warningFlag,
                                  "XBL Prototype Handler", doc,
                                  nsContentUtils::eXBL_PROPERTIES,
                                  aMessageName,
                                  params, ArrayLength(params),
                                  nsnull, EmptyString(), mLineNumber);
}

bool
nsXBLPrototypeHandler::ModifiersMatchMask(nsIDOMUIEvent* aEvent,
                                          bool aIgnoreShiftKey)
{
  nsCOMPtr<nsIDOMKeyEvent> key(do_QueryInterface(aEvent));
  nsCOMPtr<nsIDOMMouseEvent> mouse(do_QueryInterface(aEvent));

  bool keyPresent;
  if (mKeyMask & cMetaMask) {
    key ? key->GetMetaKey(&keyPresent) : mouse->GetMetaKey(&keyPresent);
    if (keyPresent != ((mKeyMask & cMeta) != 0))
      return false;
  }

  if (mKeyMask & cShiftMask && !aIgnoreShiftKey) {
    key ? key->GetShiftKey(&keyPresent) : mouse->GetShiftKey(&keyPresent);
    if (keyPresent != ((mKeyMask & cShift) != 0))
      return false;
  }

  if (mKeyMask & cAltMask) {
    key ? key->GetAltKey(&keyPresent) : mouse->GetAltKey(&keyPresent);
    if (keyPresent != ((mKeyMask & cAlt) != 0))
      return false;
  }

  if (mKeyMask & cControlMask) {
    key ? key->GetCtrlKey(&keyPresent) : mouse->GetCtrlKey(&keyPresent);
    if (keyPresent != ((mKeyMask & cControl) != 0))
      return false;
  }

  return true;
}

nsresult
nsXBLPrototypeHandler::Read(nsIScriptContext* aContext, nsIObjectInputStream* aStream)
{
  nsresult rv = aStream->Read8(&mPhase);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = aStream->Read8(&mKeyMask);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = aStream->Read8(&mType);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = aStream->Read8(&mMisc);
  NS_ENSURE_SUCCESS(rv, rv);

  PRUint32 detail; 
  rv = aStream->Read32(&detail);
  NS_ENSURE_SUCCESS(rv, rv);
  mDetail = detail;

  nsAutoString name;
  rv = aStream->ReadString(name);
  NS_ENSURE_SUCCESS(rv, rv);
  mEventName = do_GetAtom(name);

  rv = aStream->Read32(&mLineNumber);
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoString handlerText;
  rv = aStream->ReadString(handlerText);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!handlerText.IsEmpty())
    mHandlerText = ToNewUnicode(handlerText);

  return NS_OK;
}

nsresult
nsXBLPrototypeHandler::Write(nsIScriptContext* aContext, nsIObjectOutputStream* aStream)
{
  // Make sure we don't write out NS_HANDLER_TYPE_XUL types, as they are used
  // for <keyset> elements.
  if ((mType & NS_HANDLER_TYPE_XUL) || !mEventName)
    return NS_OK;

  XBLBindingSerializeDetails type = XBLBinding_Serialize_Handler;

  nsresult rv = aStream->Write8(type);
  rv = aStream->Write8(mPhase);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = aStream->Write8(mKeyMask);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = aStream->Write8(mType);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = aStream->Write8(mMisc);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = aStream->Write32(mDetail);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = aStream->WriteWStringZ(nsDependentAtomString(mEventName).get());
  NS_ENSURE_SUCCESS(rv, rv);

  rv = aStream->Write32(mLineNumber);
  NS_ENSURE_SUCCESS(rv, rv);
  return aStream->WriteWStringZ(mHandlerText ? mHandlerText : EmptyString().get());
}
