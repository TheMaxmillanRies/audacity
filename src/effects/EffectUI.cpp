/**********************************************************************

  Audacity: A Digital Audio Editor

  EffectUI.cpp

  Leland Lucius

  Audacity(R) is copyright (c) 1999-2008 Audacity Team.
  License: GPL v2 or later.  See License.txt.

**********************************************************************/


#include "EffectUI.h"

#include "widgets/BasicMenu.h"
#include "ConfigInterface.h"
#include "EffectManager.h"
#include "PluginManager.h"
#include "../ProjectHistory.h"
#include "../ProjectWindowBase.h"
#include "../TrackPanelAx.h"
#include "RealtimeEffectList.h"
#include "RealtimeEffectManager.h"
#include "widgets/wxWidgetsWindowPlacement.h"

static PluginID GetID(EffectUIHostInterface &effect)
{
   return PluginManager::GetID(&effect.GetDefinition());
}

///////////////////////////////////////////////////////////////////////////////
//
// EffectPanel
//
///////////////////////////////////////////////////////////////////////////////

class EffectPanel final : public wxPanelWrapper
{
public:
   EffectPanel(wxWindow *parent)
   :  wxPanelWrapper(parent)
   {
      // This fools NVDA into not saying "Panel" when the dialog gets focus
      SetName(TranslatableString::Inaudible);
      SetLabel(TranslatableString::Inaudible);

      mAcceptsFocus = true;
   }

   virtual ~EffectPanel()
   {
   }

   // ============================================================================
   // wxWindow implementation
   // ============================================================================

   bool AcceptsFocus() const override
   {
      return mAcceptsFocus;
   }

   // So that wxPanel is not included in Tab traversal, when required - see wxWidgets bug 15581
   bool AcceptsFocusFromKeyboard() const override
   {
      return mAcceptsFocus;
   }

   // ============================================================================
   // EffectPanel implementation
   // ============================================================================
   void SetAccept(bool accept)
   {
      mAcceptsFocus = accept;
   }

private:
   bool mAcceptsFocus;
};

///////////////////////////////////////////////////////////////////////////////
//
// EffectUIHost
//
///////////////////////////////////////////////////////////////////////////////

#include "../../images/Effect.h"
#include "../AudioIO.h"
#include "../CommonCommandFlags.h"
#include "../Menus.h"
#include "../prefs/GUISettings.h" // for RTL_WORKAROUND
#include "Project.h"
#include "../ProjectAudioManager.h"
#include "../ShuttleGui.h"
#include "ViewInfo.h"
#include "../commands/AudacityCommand.h"
#include "../commands/CommandContext.h"
#include "../widgets/AudacityMessageBox.h"
#include "../widgets/HelpSystem.h"

#include <wx/bmpbuttn.h>
#include <wx/checkbox.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>

#if defined(__WXMAC__)
#include <Cocoa/Cocoa.h>
#endif

static const int kDummyID = 20000;
static const int kSaveAsID = 20001;
static const int kImportID = 20002;
static const int kExportID = 20003;
static const int kDefaultsID = 20004;
static const int kOptionsID = 20005;
static const int kUserPresetsDummyID = 20006;
static const int kDeletePresetDummyID = 20007;
static const int kMenuID = 20100;
static const int kEnableID = 20101;
static const int kPlayID = 20102;
static const int kRewindID = 20103;
static const int kFFwdID = 20104;
static const int kPlaybackID = 20105;
static const int kCaptureID = 20106;
static const int kUserPresetsID = 21000;
static const int kDeletePresetID = 22000;
static const int kFactoryPresetsID = 23000;

BEGIN_EVENT_TABLE(EffectUIHost, wxDialogWrapper)
EVT_INIT_DIALOG(EffectUIHost::OnInitDialog)
EVT_ERASE_BACKGROUND(EffectUIHost::OnErase)
EVT_PAINT(EffectUIHost::OnPaint)
EVT_CLOSE(EffectUIHost::OnClose)
EVT_BUTTON(wxID_APPLY, EffectUIHost::OnApply)
EVT_BUTTON(wxID_CANCEL, EffectUIHost::OnCancel)
EVT_BUTTON(wxID_HELP, EffectUIHost::OnHelp)
EVT_BUTTON(eDebugID, EffectUIHost::OnDebug)
EVT_BUTTON(kMenuID, EffectUIHost::OnMenu)
EVT_CHECKBOX(kEnableID, EffectUIHost::OnEnable)
EVT_BUTTON(kPlayID, EffectUIHost::OnPlay)
EVT_BUTTON(kRewindID, EffectUIHost::OnRewind)
EVT_BUTTON(kFFwdID, EffectUIHost::OnFFwd)
EVT_MENU(kSaveAsID, EffectUIHost::OnSaveAs)
EVT_MENU(kImportID, EffectUIHost::OnImport)
EVT_MENU(kExportID, EffectUIHost::OnExport)
EVT_MENU(kOptionsID, EffectUIHost::OnOptions)
EVT_MENU(kDefaultsID, EffectUIHost::OnDefaults)
EVT_MENU_RANGE(kUserPresetsID, kUserPresetsID + 999, EffectUIHost::OnUserPreset)
EVT_MENU_RANGE(kDeletePresetID, kDeletePresetID + 999, EffectUIHost::OnDeletePreset)
EVT_MENU_RANGE(kFactoryPresetsID, kFactoryPresetsID + 999, EffectUIHost::OnFactoryPreset)
END_EVENT_TABLE()

EffectUIHost::EffectUIHost(wxWindow *parent,
   AudacityProject &project,
   EffectUIHostInterface &effect,
   EffectUIClientInterface &client,
   EffectSettingsAccess &access)
:  wxDialogWrapper(parent, wxID_ANY, effect.GetDefinition().GetName(),
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMINIMIZE_BOX | wxMAXIMIZE_BOX)
, mEffectUIHost{ effect }
, mClient{ client }
// Grab a pointer to the access object,
// extending its lifetime while this remains:
, mpAccess{ access.shared_from_this() }
, mProject{ project }
{
#if defined(__WXMAC__)
   // Make sure the effect window actually floats above the main window
   [ [((NSView *)GetHandle()) window] setLevel:NSFloatingWindowLevel];
#endif
   
   SetName( effect.GetDefinition().GetName() );

   // This style causes Validate() and TransferDataFromWindow() to visit
   // sub-windows recursively, applying any wxValidators
   SetExtraStyle(GetExtraStyle() | wxWS_EX_VALIDATE_RECURSIVELY);
   
   mParent = parent;
   mClient = client;
   
   mInitialized = false;
   mSupportsRealtime = false;
   
   mDisableTransport = false;
   
   mEnabled = true;
   
   mPlayPos = 0.0;
}

EffectUIHost::~EffectUIHost()
{
   wxASSERT(mClosed);
}

// ============================================================================
// wxWindow implementation
// ============================================================================

bool EffectUIHost::TransferDataToWindow()
{
   // Transfer-to takes const reference to settings
   return mEffectUIHost.TransferDataToWindow(mpAccess->Get()) &&
      //! Do other apperance updates
      mpValidator->UpdateUI() &&
      //! Do validators
      wxDialogWrapper::TransferDataToWindow();
 
}

bool EffectUIHost::TransferDataFromWindow()
{
   //! Do validations of any wxValidator objects
   if (!wxDialogWrapper::Validate())
      return false;

   //! Do transfers of any wxValidator objects
   if (!wxDialogWrapper::TransferDataFromWindow())
      return false;

   //! Do other custom validation and transfer actions
   if (!mpValidator->ValidateUI())
      return false;
   
   // Transfer-from takes non-const reference to settings
   bool result = true;
   mpAccess->ModifySettings([&](EffectSettings &settings){
      // Allow other transfers, and reassignment of settings
      result = mEffectUIHost.TransferDataFromWindow(settings);
   });
   return result;
}

// ============================================================================
// wxDialog implementation
// ============================================================================

int EffectUIHost::ShowModal()
{
#if defined(__WXMSW__)
   // Swap the Close and Apply buttons
   wxSizer *sz = mApplyBtn->GetContainingSizer();
   wxASSERT(mApplyBtn->GetParent()); // To justify safenew
   wxButton *apply = safenew wxButton(mApplyBtn->GetParent(), wxID_APPLY);
   sz->Replace(mCloseBtn, apply);
   sz->Replace(mApplyBtn, mCloseBtn);
   sz->Layout();
   mApplyBtn->Destroy();
   mApplyBtn = apply;
   mApplyBtn->SetDefault();
   mApplyBtn->SetLabel(wxGetStockLabel(wxID_OK, 0));
   mCloseBtn->SetLabel(wxGetStockLabel(wxID_CANCEL, 0));
#else
   mApplyBtn->SetLabel(wxGetStockLabel(wxID_OK));
   mCloseBtn->SetLabel(wxGetStockLabel(wxID_CANCEL));
#endif
   
   Layout();
   
   return wxDialogWrapper::ShowModal();
}

// ============================================================================
// EffectUIHost implementation
// ============================================================================

wxPanel *EffectUIHost::BuildButtonBar(wxWindow *parent)
{
   mSupportsRealtime = mEffectUIHost.GetDefinition().SupportsRealtime();
   mIsGUI = mClient.IsGraphicalUI();
   mIsBatch = mEffectUIHost.IsBatchProcessing();

   int margin = 0;
#if defined(__WXMAC__)
   margin = 3; // I'm sure it's needed because of the order things are created...
#endif

   const auto bar = safenew wxPanelWrapper(parent, wxID_ANY);

   // This fools NVDA into not saying "Panel" when the dialog gets focus
   bar->SetName(TranslatableString::Inaudible);
   bar->SetLabel(TranslatableString::Inaudible);

   ShuttleGui S{ bar, eIsCreating,
      false /* horizontal */,
      { -1, -1 } /* minimum size */
   };
   {
      S.SetBorder( margin );

      if (!mIsGUI)
      {
         mMenuBtn = S.Id( kMenuID )
            .ToolTip(XO("Manage presets and options"))
            .AddButton( XXO("&Manage"), wxALIGN_CENTER | wxTOP | wxBOTTOM );
      }
      else
      {
         mMenuBtn = S.Id( kMenuID )
            .ToolTip(XO("Manage presets and options"))
            .Name(XO("&Manage"))
            .AddBitmapButton( CreateBitmap(effect_menu_xpm, true, true) );
         mMenuBtn->SetBitmapPressed(CreateBitmap(effect_menu_xpm, false, true));
      }

      S.AddSpace( 5, 5 );

      if (!mIsBatch)
      {
         if (!mIsGUI)
         {
            if (mSupportsRealtime)
            {
               mPlayToggleBtn = S.Id( kPlayID )
                  .ToolTip(XO("Start and stop playback"))
                  .AddButton( XXO("Start &Playback"),
                              wxALIGN_CENTER | wxTOP | wxBOTTOM );
            }
            else if (
               (mEffectUIHost.GetDefinition().GetType() != EffectTypeAnalyze) &&
               (mEffectUIHost.GetDefinition().GetType() != EffectTypeTool) )
            {
               mPlayToggleBtn = S.Id( kPlayID )
                  .ToolTip(XO("Preview effect"))
                  .AddButton( XXO("&Preview"),
                              wxALIGN_CENTER | wxTOP | wxBOTTOM );
            }
         }
         else
         {
            mPlayBM = CreateBitmap(effect_play_xpm, true, false);
            mPlayDisabledBM = CreateBitmap(effect_play_disabled_xpm, true, true);
            mStopBM = CreateBitmap(effect_stop_xpm, true, false);
            mStopDisabledBM = CreateBitmap(effect_stop_disabled_xpm, true, false);
            mPlayBtn = S.Id( kPlayID ).AddBitmapButton( mPlayBM );
            mPlayBtn->SetBitmapDisabled(mPlayDisabledBM);
            mPlayBtn->SetBitmapPressed(CreateBitmap(effect_play_xpm, false, true));
            if (!mSupportsRealtime)
            {
               mPlayBtn->SetToolTip(_("Preview effect"));
#if defined(__WXMAC__)
               mPlayBtn->SetName(_("Preview effect"));
#else
               mPlayBtn->SetLabel(_("&Preview effect"));
#endif
            }
         }

         if (mSupportsRealtime)
         {
            if (!mIsGUI)
            {
               mRewindBtn = S.Id( kRewindID )
                  .ToolTip(XO("Skip backward"))
                  .AddButton( XXO("Skip &Backward"),
                              wxALIGN_CENTER | wxTOP | wxBOTTOM );
            }
            else
            {
               mRewindBtn = S.Id( kRewindID )
                  .ToolTip(XO("Skip backward"))
                  .Name(XO("Skip &Backward"))
                  .AddBitmapButton( CreateBitmap(
                     effect_rewind_xpm, true, true) );
               mRewindBtn->SetBitmapDisabled(
                     CreateBitmap(effect_rewind_disabled_xpm, true, false));
               mRewindBtn->SetBitmapPressed(CreateBitmap(effect_rewind_xpm, false, true));
            }

            if (!mIsGUI)
            {
               mFFwdBtn = S.Id( kFFwdID )
                  .ToolTip(XO("Skip forward"))
                  .AddButton( XXO("Skip &Forward"),
                     wxALIGN_CENTER | wxTOP | wxBOTTOM );
            }
            else
            {
               mFFwdBtn = S.Id( kFFwdID )
                  .ToolTip(XO("Skip forward"))
                  .Name(XO("Skip &Forward"))
                  .AddBitmapButton( CreateBitmap(
                     effect_ffwd_xpm, true, true) );
               mFFwdBtn->SetBitmapDisabled(
                  CreateBitmap(effect_ffwd_disabled_xpm, true, false));
               mFFwdBtn->SetBitmapPressed(CreateBitmap(effect_ffwd_xpm, false, true));
            }

            S.AddSpace( 5, 5 );

            mEnableCb = S.Id( kEnableID )
               .Position(wxALIGN_CENTER | wxTOP | wxBOTTOM)
               .Name(XO("Enable"))
               .AddCheckBox( XXO("&Enable"), mEnabled );
            //
         }
      }
   }

   bar->GetSizer()->SetSizeHints( bar );

   return bar;
}

bool EffectUIHost::Initialize()
{
   {
      auto gAudioIO = AudioIO::Get();
      mDisableTransport = !gAudioIO->IsAvailable(mProject);
      mPlaying = gAudioIO->IsStreamActive(); // not exactly right, but will suffice
      mCapturing = gAudioIO->IsStreamActive() && gAudioIO->GetNumCaptureChannels() > 0 && !gAudioIO->IsMonitoring();
   }

   // Build a "host" dialog, framing a panel that the client fills in.
   // The frame includes buttons to preview, apply, load and save presets, etc.
   EffectPanel *w {};
   ShuttleGui S{ this, eIsCreating };
   {
      S.StartHorizontalLay( wxEXPAND );
      {
         // Make the panel for the client
         Destroy_ptr<EffectPanel> uw{ safenew EffectPanel( S.GetParent() ) };
         RTL_WORKAROUND(uw.get());

         // Try to give the window a sensible default/minimum size
         uw->SetMinSize(wxSize(wxMax(600, mParent->GetSize().GetWidth() * 2 / 3),
            mParent->GetSize().GetHeight() / 2));

         // Let the client add things to the panel
         ShuttleGui S1{ uw.get(), eIsCreating };
         mpValidator = mClient.PopulateUI(S1, *mpAccess);
         if (!mpValidator)
            return false;

         S.Prop( 1 )
            .Position(wxEXPAND)
            .AddWindow((w = uw.release()));
      }
      S.EndHorizontalLay();

      S.StartPanel();
      {
         const auto bar = BuildButtonBar( S.GetParent() );

         long buttons;
         if ( mEffectUIHost.GetDefinition().ManualPage().empty() && mEffectUIHost.GetDefinition().HelpPage().empty()) {
            buttons = eApplyButton | eCloseButton;
            this->SetAcceleratorTable(wxNullAcceleratorTable);
         }
         else {
            buttons = eApplyButton | eCloseButton | eHelpButton;
            wxAcceleratorEntry entries[1];
#if defined(__WXMAC__)
            // Is there a standard shortcut on Mac?
#else
            entries[0].Set(wxACCEL_NORMAL, (int) WXK_F1, wxID_HELP);
#endif
            wxAcceleratorTable accel(1, entries);
            this->SetAcceleratorTable(accel);
         }

         if (mEffectUIHost.GetDefinition().EnablesDebug())
            buttons |= eDebugButton;

         S.AddStandardButtons(buttons, bar);
      }
      S.EndPanel();
   }

   Layout();
   Fit();
   Center();

   mApplyBtn = (wxButton *) FindWindow(wxID_APPLY);
   mCloseBtn = (wxButton *) FindWindow(wxID_CANCEL);

   UpdateControls();

   w->SetAccept(!mIsGUI);
   (!mIsGUI ? w : FindWindow(wxID_APPLY))->SetFocus();

   LoadUserPresets();

   InitializeRealtime();

   SetMinSize(GetSize());
   return true;
}

void EffectUIHost::OnInitDialog(wxInitDialogEvent & evt)
{
   // Do default handling
   wxDialogWrapper::OnInitDialog(evt);
   
#if wxCHECK_VERSION(3, 0, 0)
   //#warning "check to see if this still needed in wx3"
#endif
   
   // Pure hackage coming down the pike...
   //
   // I have no idea why, but if a wxTextCtrl is the first control in the
   // panel, then its contents will not be automatically selected when the
   // dialog is displayed.
   //
   // So, we do the selection manually.
   wxTextCtrl *focused = wxDynamicCast(FindFocus(), wxTextCtrl);
   if (focused)
   {
      focused->SelectAll();
   }
}

void EffectUIHost::OnErase(wxEraseEvent & WXUNUSED(evt))
{
   // Ignore it
}

void EffectUIHost::OnPaint(wxPaintEvent & WXUNUSED(evt))
{
   wxPaintDC dc(this);
   
   dc.Clear();
}

void EffectUIHost::OnClose(wxCloseEvent & WXUNUSED(evt))
{
   DoCancel();
   
   CleanupRealtime();
   
   Hide();

   mSuspensionScope.reset();
   mpValidator.reset();

   Destroy();
#if wxDEBUG_LEVEL
   mClosed = true;
#endif
}

void EffectUIHost::OnApply(wxCommandEvent & evt)
{
   auto &project = mProject;

   // On wxGTK (wx2.8.12), the default action is still executed even if
   // the button is disabled.  This appears to affect all wxDialogs, not
   // just our Effects dialogs.  So, this is a only temporary workaround
   // for legacy effects that disable the OK button.  Hopefully this has
   // been corrected in wx3.
   if (!FindWindow(wxID_APPLY)->IsEnabled())
   {
      return;
   }
   
   // Honor the "select all if none" preference...a little hackish, but whatcha gonna do...
   if (!mIsBatch &&
       mEffectUIHost.GetDefinition().GetType() != EffectTypeGenerate &&
       mEffectUIHost.GetDefinition().GetType() != EffectTypeTool &&
       ViewInfo::Get( project ).selectedRegion.isPoint())
   {
      auto flags = AlwaysEnabledFlag;
      bool allowed =
      MenuManager::Get( project ).ReportIfActionNotAllowed(
         mEffectUIHost.GetDefinition().GetName(),
         flags,
         WaveTracksSelectedFlag() | TimeSelectedFlag());
      if (!allowed)
         return;
   }
   
   if (!TransferDataFromWindow() ||
       !mEffectUIHost.GetDefinition()
         .SaveUserPreset(CurrentSettingsGroup(), mpAccess->Get()))
      return;

   if (IsModal())
   {
      mDismissed = true;
      
      EndModal(evt.GetId());
      
      Close();
      
      return;
   }
   
   // Progress dialog no longer yields, so this "shouldn't" be necessary (yet to be proven
   // for sure), but it is a nice visual cue that something is going on.
   mApplyBtn->Disable();
   auto cleanup = finally( [&] { mApplyBtn->Enable(); } );

   CommandContext context( project );
   // This is absolute hackage...but easy and I can't think of another way just now.
   //
   // It should callback to the EffectManager to kick off the processing
   EffectUI::DoEffect(GetID(mEffectUIHost), context,
      EffectManager::kConfigured);
}

void EffectUIHost::DoCancel()
{
   if (!mDismissed) {
      if (IsModal())
         EndModal(0);
      else
         Hide();
      
      mDismissed = true;
   }
}

void EffectUIHost::OnCancel(wxCommandEvent & WXUNUSED(evt))
{
   DoCancel();
   Close();
}

void EffectUIHost::OnHelp(wxCommandEvent & WXUNUSED(event))
{
   if (mEffectUIHost.GetDefinition().ManualPage().empty()) {
      // No manual page, so use help page

      const auto &helpPage = mEffectUIHost.GetDefinition().HelpPage();
      // It was guaranteed in Initialize() that there is no help
      // button when neither manual nor help page is specified, so this
      // event handler it not reachable in that case.
      wxASSERT(!helpPage.empty());

      // Old ShowHelp required when there is no on-line manual.
      // Always use default web browser to allow full-featured HTML pages.
      HelpSystem::ShowHelp(FindWindow(wxID_HELP), helpPage, wxEmptyString, true, true);
   }
   else {
      // otherwise use the NEW ShowHelp
      HelpSystem::ShowHelp(FindWindow(wxID_HELP), mEffectUIHost.GetDefinition().ManualPage(), true);
   }
}

void EffectUIHost::OnDebug(wxCommandEvent & evt)
{
   OnApply(evt);
}

void EffectUIHost::OnMenu(wxCommandEvent & WXUNUSED(evt))
{
   wxMenu menu;
   menu.Bind(wxEVT_MENU, [](auto&){}, kUserPresetsDummyID);
   menu.Bind(wxEVT_MENU, [](auto&){}, kDeletePresetDummyID);
   LoadUserPresets();
   
   if (mUserPresets.size() == 0)
   {
      menu.Append(kUserPresetsDummyID, _("User Presets"))->Enable(false);
   }
   else
   {
      auto sub = std::make_unique<wxMenu>();
      for (size_t i = 0, cnt = mUserPresets.size(); i < cnt; i++)
      {
         sub->Append(kUserPresetsID + i, mUserPresets[i]);
      }
      menu.Append(0, _("User Presets"), sub.release());
   }
   
   menu.Append(kSaveAsID, _("Save Preset..."));
   
   if (mUserPresets.size() == 0)
   {
      menu.Append(kDeletePresetDummyID, _("Delete Preset"))->Enable(false);
   }
   else
   {
      auto sub = std::make_unique<wxMenu>();
      for (size_t i = 0, cnt = mUserPresets.size(); i < cnt; i++)
      {
         sub->Append(kDeletePresetID + i, mUserPresets[i]);
      }
      menu.Append(0, _("Delete Preset"), sub.release());
   }
   
   menu.AppendSeparator();
   
   auto factory = mEffectUIHost.GetDefinition().GetFactoryPresets();
   
   {
      auto sub = std::make_unique<wxMenu>();
      sub->Append(kDefaultsID, _("Defaults"));
      if (factory.size() > 0)
      {
         sub->AppendSeparator();
         for (size_t i = 0, cnt = factory.size(); i < cnt; i++)
         {
            auto label = factory[i];
            if (label.empty())
            {
               label = _("None");
            }
            
            sub->Append(kFactoryPresetsID + i, label);
         }
      }
      menu.Append(0, _("Factory Presets"), sub.release());
   }
   
   menu.AppendSeparator();
   menu.Append(kImportID, _("Import..."))->Enable(mClient.CanExportPresets());
   menu.Append(kExportID, _("Export..."))->Enable(mClient.CanExportPresets());
   menu.AppendSeparator();
   menu.Append(kOptionsID, _("Options..."))->Enable(mClient.HasOptions());
   menu.AppendSeparator();
   
   {
      auto sub = std::make_unique<wxMenu>();
      
      auto &definition = mEffectUIHost.GetDefinition();
      sub->Append(kDummyID, wxString::Format(_("Type: %s"),
         ::wxGetTranslation( definition.GetFamily().Translation() )));
      sub->Append(kDummyID, wxString::Format(_("Name: %s"), definition.GetName().Translation()));
      sub->Append(kDummyID, wxString::Format(_("Version: %s"), definition.GetVersion()));
      sub->Append(kDummyID, wxString::Format(_("Vendor: %s"), definition.GetVendor().Translation()));
      sub->Append(kDummyID, wxString::Format(_("Description: %s"), definition.GetDescription().Translation()));
      sub->Bind(wxEVT_MENU, [](auto&){}, kDummyID);

      menu.Append(0, _("About"), sub.release());
   }
   
   wxWindow *btn = FindWindow(kMenuID);
   wxRect r = btn->GetRect();
   BasicMenu::Handle{ &menu }.Popup(
      wxWidgetsWindowPlacement{ btn },
      { r.GetLeft(), r.GetBottom() }
   );
}

void EffectUIHost::OnEnable(wxCommandEvent & WXUNUSED(evt))
{
   mEnabled = mEnableCb->GetValue();
   
   if (mEnabled)
      mSuspensionScope.reset();
   else
      mSuspensionScope.emplace(AudioIO::Get()->SuspensionScope());

   UpdateControls();
}

void EffectUIHost::OnPlay(wxCommandEvent & WXUNUSED(evt))
{
   if (!mSupportsRealtime)
   {
      if (!TransferDataFromWindow())
         return;
      
      mEffectUIHost.Preview(*mpAccess, false);
      
      return;
   }
   
   if (mPlaying)
   {
      auto gAudioIO = AudioIO::Get();
      mPlayPos = gAudioIO->GetStreamTime();
      auto &projectAudioManager = ProjectAudioManager::Get( mProject );
      projectAudioManager.Stop();
   }
   else
   {
      auto &viewInfo = ViewInfo::Get( mProject );
      const auto &selectedRegion = viewInfo.selectedRegion;
      const auto &playRegion = viewInfo.playRegion;
      if ( playRegion.Active() )
      {
         mRegion.setTimes(playRegion.GetStart(), playRegion.GetEnd());
         mPlayPos = mRegion.t0();
      }
      else if (selectedRegion.t0() != mRegion.t0() ||
               selectedRegion.t1() != mRegion.t1())
      {
         mRegion = selectedRegion;
         mPlayPos = mRegion.t0();
      }
      
      if (mPlayPos > mRegion.t1())
      {
         mPlayPos = mRegion.t1();
      }
      
      auto &projectAudioManager = ProjectAudioManager::Get( mProject );
      projectAudioManager.PlayPlayRegion(
                                         SelectedRegion(mPlayPos, mRegion.t1()),
                                         DefaultPlayOptions( mProject ),
                                         PlayMode::normalPlay );
   }
}

void EffectUIHost::OnRewind(wxCommandEvent & WXUNUSED(evt))
{
   if (mPlaying)
   {
      auto gAudioIO = AudioIO::Get();
      double seek;
      gPrefs->Read(wxT("/AudioIO/SeekShortPeriod"), &seek, 1.0);
      
      double pos = gAudioIO->GetStreamTime();
      if (pos - seek < mRegion.t0())
      {
         seek = pos - mRegion.t0();
      }
      
      gAudioIO->SeekStream(-seek);
   }
   else
   {
      mPlayPos = mRegion.t0();
   }
}

void EffectUIHost::OnFFwd(wxCommandEvent & WXUNUSED(evt))
{
   if (mPlaying)
   {
      double seek;
      gPrefs->Read(wxT("/AudioIO/SeekShortPeriod"), &seek, 1.0);
      
      auto gAudioIO = AudioIO::Get();
      double pos = gAudioIO->GetStreamTime();
      if (mRegion.t0() < mRegion.t1() && pos + seek > mRegion.t1())
      {
         seek = mRegion.t1() - pos;
      }
      
      gAudioIO->SeekStream(seek);
   }
   else
   {
      // It allows to play past end of selection...probably useless
      mPlayPos = mRegion.t1();
   }
}

void EffectUIHost::OnPlayback(AudioIOEvent evt)
{
   if (evt.on)
   {
      if (evt.pProject != &mProject)
      {
         mDisableTransport = true;
      }
      else
      {
         mPlaying = true;
      }
   }
   else
   {
      mDisableTransport = false;
      mPlaying = false;
   }
   
   if (mPlaying)
   {
      mRegion = ViewInfo::Get( mProject ).selectedRegion;
      mPlayPos = mRegion.t0();
   }
   
   UpdateControls();
}

void EffectUIHost::OnCapture(AudioIOEvent evt)
{
   if (evt.on)
   {
      if (evt.pProject != &mProject)
      {
         mDisableTransport = true;
      }
      else
      {
         mCapturing = true;
      }
   }
   else
   {
      mDisableTransport = false;
      mCapturing = false;
   }
   
   UpdateControls();
}

void EffectUIHost::OnUserPreset(wxCommandEvent & evt)
{
   int preset = evt.GetId() - kUserPresetsID;
   
   mpAccess->ModifySettings([&](EffectSettings &settings){
      mEffectUIHost.GetDefinition()
         .LoadUserPreset(UserPresetsGroup(mUserPresets[preset]), settings);
      TransferDataToWindow();
   });
   return;
}

void EffectUIHost::OnFactoryPreset(wxCommandEvent & evt)
{
   mpAccess->ModifySettings([&](EffectSettings &settings){
      mEffectUIHost.GetDefinition()
         .LoadFactoryPreset(evt.GetId() - kFactoryPresetsID, settings);
      TransferDataToWindow();
   });
   return;
}

void EffectUIHost::OnDeletePreset(wxCommandEvent & evt)
{
   auto preset = mUserPresets[evt.GetId() - kDeletePresetID];
   
   int res = AudacityMessageBox(
                                XO("Are you sure you want to delete \"%s\"?").Format( preset ),
                                XO("Delete Preset"),
                                wxICON_QUESTION | wxYES_NO);
   if (res == wxYES)
   {
      RemoveConfigSubgroup(mEffectUIHost.GetDefinition(),
         PluginSettings::Private, UserPresetsGroup(preset));
   }
   
   LoadUserPresets();
   
   return;
}

void EffectUIHost::OnSaveAs(wxCommandEvent & WXUNUSED(evt))
{
   wxTextCtrl *text;
   wxString name;
   wxDialogWrapper dlg(this, wxID_ANY, XO("Save Preset"));
   
   ShuttleGui S(&dlg, eIsCreating);
   
   S.StartPanel();
   {
      S.StartVerticalLay(1);
      {
         S.StartHorizontalLay(wxALIGN_LEFT, 0);
         {
            text = S.AddTextBox(XXO("Preset name:"), name, 30);
         }
         S.EndHorizontalLay();
         S.SetBorder(10);
         S.AddStandardButtons();
      }
      S.EndVerticalLay();
   }
   S.EndPanel();
   
   dlg.SetSize(dlg.GetSizer()->GetMinSize());
   dlg.Center();
   dlg.Fit();
   
   while (true)
   {
      int rc = dlg.ShowModal();
      
      if (rc != wxID_OK)
      {
         break;
      }
      
      name = text->GetValue();
      if (name.empty())
      {
         AudacityMessageDialog md(
                                  this,
                                  XO("You must specify a name"),
                                  XO("Save Preset") );
         md.Center();
         md.ShowModal();
         continue;
      }
      
      if ( make_iterator_range( mUserPresets ).contains( name ) )
      {
         AudacityMessageDialog md(
                                  this,
                                  XO("Preset already exists.\n\nReplace?"),
                                  XO("Save Preset"),
                                  wxYES_NO | wxCANCEL | wxICON_EXCLAMATION );
         md.Center();
         int choice = md.ShowModal();
         if (choice == wxID_CANCEL)
         {
            break;
         }
         
         if (choice == wxID_NO)
         {
            continue;
         }
      }
      
      if (TransferDataFromWindow())
         mEffectUIHost.GetDefinition()
            .SaveUserPreset(UserPresetsGroup(name), mpAccess->Get());
      LoadUserPresets();
      
      break;
   }
   
   return;
}

void EffectUIHost::OnImport(wxCommandEvent & WXUNUSED(evt))
{
   mpAccess->ModifySettings([&](EffectSettings &settings){
      mClient.ImportPresets(settings);
      TransferDataToWindow();
   });
   LoadUserPresets();

   return;
}

void EffectUIHost::OnExport(wxCommandEvent & WXUNUSED(evt))
{
   // may throw
   // exceptions are handled in AudacityApp::OnExceptionInMainLoop
   if (TransferDataFromWindow())
     mClient.ExportPresets(mpAccess->Get());
   
   return;
}

void EffectUIHost::OnOptions(wxCommandEvent & WXUNUSED(evt))
{
   mClient.ShowOptions();
   
   return;
}

void EffectUIHost::OnDefaults(wxCommandEvent & WXUNUSED(evt))
{
   mpAccess->ModifySettings([&](EffectSettings &settings){
      mEffectUIHost.GetDefinition().LoadFactoryDefaults(settings);
      TransferDataToWindow();
   });
   return;
}

wxBitmap EffectUIHost::CreateBitmap(const char * const xpm[], bool up, bool pusher)
{
   wxMemoryDC dc;
   wxBitmap pic(xpm);
   
   wxBitmap mod(pic.GetWidth() + 6, pic.GetHeight() + 6, 24);
   dc.SelectObject(mod);
   
#if defined(__WXGTK__)
   wxColour newColour = wxSystemSettings::GetColour(wxSYS_COLOUR_BACKGROUND);
#else
   wxColour newColour = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
#endif
   
   dc.SetBackground(wxBrush(newColour));
   dc.Clear();
   
   int offset = 3;
   if (pusher)
   {
      if (!up)
      {
         offset += 1;
      }
   }
   
   dc.DrawBitmap(pic, offset, offset, true);
   
   dc.SelectObject(wxNullBitmap);
   
   return mod;
}

void EffectUIHost::UpdateControls()
{
   if (mIsBatch)
   {
      return;
   }

   if (mCapturing || mDisableTransport)
   {
      // Don't allow focus to get trapped
      wxWindow *focus = FindFocus();
      if (focus == mRewindBtn || focus == mFFwdBtn || focus == mPlayBtn || focus == mEnableCb)
      {
         mCloseBtn->SetFocus();
      }
   }
   
   mApplyBtn->Enable(!mCapturing);
   if ((mEffectUIHost.GetDefinition().GetType() != EffectTypeAnalyze) &&
       (mEffectUIHost.GetDefinition().GetType() != EffectTypeTool) )
   {
      (!mIsGUI ? mPlayToggleBtn : mPlayBtn)->Enable(!(mCapturing || mDisableTransport));
   }
   
   if (mSupportsRealtime)
   {
      mRewindBtn->Enable(!(mCapturing || mDisableTransport));
      mFFwdBtn->Enable(!(mCapturing || mDisableTransport));
      mEnableCb->Enable(!(mCapturing || mDisableTransport));
      
      wxBitmapButton *bb;
      
      if (mPlaying)
      {
         if (!mIsGUI)
         {
            /* i18n-hint: The access key "&P" should be the same in
             "Stop &Playback" and "Start &Playback" */
            mPlayToggleBtn->SetLabel(_("Stop &Playback"));
            mPlayToggleBtn->Refresh();
         }
         else
         {
            bb = (wxBitmapButton *) mPlayBtn;
            bb->SetBitmapLabel(mStopBM);
            bb->SetBitmapDisabled(mStopDisabledBM);
            bb->SetToolTip(_("Stop"));
#if defined(__WXMAC__)
            bb->SetName(_("Stop &Playback"));
#else
            bb->SetLabel(_("Stop &Playback"));
#endif
         }
      }
      else
      {
         if (!mIsGUI)
         {
            /* i18n-hint: The access key "&P" should be the same in
             "Stop &Playback" and "Start &Playback" */
            mPlayToggleBtn->SetLabel(_("Start &Playback"));
            mPlayToggleBtn->Refresh();
         }
         else
         {
            bb = (wxBitmapButton *) mPlayBtn;
            bb->SetBitmapLabel(mPlayBM);
            bb->SetBitmapDisabled(mPlayDisabledBM);
            bb->SetToolTip(_("Play"));
#if defined(__WXMAC__)
            bb->SetName(_("Start &Playback"));
#else
            bb->SetLabel(_("Start &Playback"));
#endif
         }
      }
   }
}

void EffectUIHost::LoadUserPresets()
{
   mUserPresets.clear();
   
   GetConfigSubgroups(mEffectUIHost.GetDefinition(),
      PluginSettings::Private, UserPresetsGroup(wxEmptyString), mUserPresets);
   
   std::sort( mUserPresets.begin(), mUserPresets.end() );
   
   return;
}

void EffectUIHost::InitializeRealtime()
{
   if (mSupportsRealtime && !mInitialized) {
      mpState =
         AudioIO::Get()->AddState(mProject, nullptr, GetID(mEffectUIHost));
      /*
      ProjectHistory::Get(mProject).PushState(
         XO("Added %s effect").Format(mpState->GetEffect()->GetName()),
         XO("Added Effect"),
         UndoPush::NONE
      );
       */
      AudioIO::Get()->Subscribe([this](AudioIOEvent event){
         switch (event.type) {
         case AudioIOEvent::PLAYBACK:
            OnPlayback(event); break;
         case AudioIOEvent::CAPTURE:
            OnCapture(event); break;
         default:
            break;
         }
      });
      
      mInitialized = true;
   }
}

void EffectUIHost::CleanupRealtime()
{
   if (mSupportsRealtime && mInitialized) {
      if (mpState) {
         AudioIO::Get()->RemoveState(mProject, nullptr, *mpState);
      /*
         ProjectHistory::Get(mProject).PushState(
            XO("Removed %s effect").Format(mpState->GetEffect()->GetName()),
            XO("Removed Effect"),
            UndoPush::NONE
         );
       */
      }
      mInitialized = false;
   }
}

wxDialog *EffectUI::DialogFactory( wxWindow &parent,
   EffectUIHostInterface &host,
   EffectUIClientInterface &client,
   EffectSettingsAccess &access)
{
   // Make sure there is an associated project, whose lifetime will
   // govern the lifetime of the dialog, even when the dialog is
   // non-modal, as for realtime effects
   auto project = FindProjectFromWindow(&parent);
   if ( !project )
      return nullptr;

   Destroy_ptr<EffectUIHost> dlg{
      safenew EffectUIHost{ &parent, *project, host, client, access } };
   
   if (dlg->Initialize())
   {
      // release() is safe because parent will own it
      return dlg.release();
   }
   
   return nullptr;
};

#include "PluginManager.h"
#include "ProjectRate.h"
#include "../ProjectWindow.h"
#include "../SelectUtilities.h"
#include "../TrackPanel.h"
#include "../WaveTrack.h"
#include "../commands/CommandManager.h"

/// DoEffect() takes a PluginID and executes the associated effect.
///
/// At the moment flags are used only to indicate whether to prompt for
//  parameters, whether to save the state to history and whether to allow
/// 'Repeat Last Effect'.

/* static */ bool EffectUI::DoEffect(
   const PluginID & ID, const CommandContext &context, unsigned flags )
{
   AudacityProject &project = context.project;
   auto &tracks = TrackList::Get( project );
   auto &trackPanel = TrackPanel::Get( project );
   auto &trackFactory = WaveTrackFactory::Get( project );
   auto rate = ProjectRate::Get(project).GetRate();
   auto &selectedRegion = ViewInfo::Get( project ).selectedRegion;
   auto &commandManager = CommandManager::Get( project );
   auto &window = ProjectWindow::Get( project );

   const PluginDescriptor *plug = PluginManager::Get().GetPlugin(ID);
   if (!plug)
      return false;

   EffectType type = plug->GetEffectType();

   // Make sure there's no activity since the effect is about to be applied
   // to the project's tracks.  Mainly for Apply during RTP, but also used
   // for batch commands
   if (flags & EffectManager::kConfigured)
   {
      ProjectAudioManager::Get( project ).Stop();
      //Don't Select All if repeating Generator Effect
      if (!(flags & EffectManager::kConfigured)) {
         SelectUtilities::SelectAllIfNone(project);
      }
   }

   auto nTracksOriginally = tracks.size();
   wxWindow *focus = wxWindow::FindFocus();
   wxWindow *parent = nullptr;
   if (focus != nullptr) {
      parent = focus->GetParent();
   }

   bool success = false;
   auto cleanup = finally( [&] {

      if (!success) {
         // For now, we're limiting realtime preview to a single effect, so
         // make sure the menus reflect that fact that one may have just been
         // opened.
         MenuManager::Get(project).UpdateMenus( false );
      }

   } );

   int count = 0;
   bool clean = true;
   for (auto t : tracks.Selected< const WaveTrack >()) {
      if (t->GetEndTime() != 0.0)
         clean = false;
      count++;
   }

   EffectManager & em = EffectManager::Get();

   em.SetSkipStateFlag( false );
   success = false;
   if (auto effect = em.GetEffect(ID)) {
      if (auto pSettings = em.GetDefaultSettings(ID)) {
         const auto pAccess =
            std::make_shared<SimpleEffectSettingsAccess>(*pSettings);
         pAccess->ModifySettings([&](EffectSettings &settings){
            success = effect->DoEffect(settings,
               rate,
               &tracks,
               &trackFactory,
               selectedRegion,
               flags,
               &window,
               (flags & EffectManager::kConfigured) == 0
                  ? DialogFactory
                  : nullptr,
               pAccess);
         });
      }
   }

   if (!success)
      return false;

   if (em.GetSkipStateFlag())
      flags = flags | EffectManager::kSkipState;

   if (!(flags & EffectManager::kSkipState))
   {
      auto shortDesc = em.GetCommandName(ID);
      auto longDesc = em.GetCommandDescription(ID);
      ProjectHistory::Get( project ).PushState(longDesc, shortDesc);
   }

   if (!(flags & EffectManager::kDontRepeatLast))
   {
      // Remember a successful generator, effect, analyzer, or tool Process
         auto shortDesc = em.GetCommandName(ID);
         /* i18n-hint: %s will be the name of the effect which will be
          * repeated if this menu item is chosen */
         auto lastEffectDesc = XO("Repeat %s").Format(shortDesc);
         auto& menuManager = MenuManager::Get(project);
         switch ( type ) {
         case EffectTypeGenerate:
            commandManager.Modify(wxT("RepeatLastGenerator"), lastEffectDesc);
            menuManager.mLastGenerator = ID;
            menuManager.mRepeatGeneratorFlags = EffectManager::kConfigured;
            break;
         case EffectTypeProcess:
            commandManager.Modify(wxT("RepeatLastEffect"), lastEffectDesc);
            menuManager.mLastEffect = ID;
            menuManager.mRepeatEffectFlags = EffectManager::kConfigured;
            break;
         case EffectTypeAnalyze:
            commandManager.Modify(wxT("RepeatLastAnalyzer"), lastEffectDesc);
            menuManager.mLastAnalyzer = ID;
            menuManager.mLastAnalyzerRegistration = MenuCreator::repeattypeplugin;
            menuManager.mRepeatAnalyzerFlags = EffectManager::kConfigured;
            break;
         case EffectTypeTool:
            commandManager.Modify(wxT("RepeatLastTool"), lastEffectDesc);
            menuManager.mLastTool = ID;
            menuManager.mLastToolRegistration = MenuCreator::repeattypeplugin;
            menuManager.mRepeatToolFlags = EffectManager::kConfigured;
            if (shortDesc == NYQUIST_PROMPT_NAME) {
               menuManager.mRepeatToolFlags = EffectManager::kRepeatNyquistPrompt;  //Nyquist Prompt is not configured
            }
            break;
      }
   }

   //STM:
   //The following automatically re-zooms after sound was generated.
   // IMO, it was disorienting, removing to try out without re-fitting
   //mchinen:12/14/08 reapplying for generate effects
   if (type == EffectTypeGenerate)
   {
      if (count == 0 || (clean && selectedRegion.t0() == 0.0))
         window.DoZoomFit();
         //  trackPanel->Refresh(false);
   }

   // PRL:  RedrawProject explicitly because sometimes history push is skipped
   window.RedrawProject();

   if (focus != nullptr && focus->GetParent()==parent) {
      focus->SetFocus();
   }

   // A fix for Bug 63
   // New tracks added?  Scroll them into view so that user sees them.
   // Don't care what track type.  An analyser might just have added a
   // Label track and we want to see it.
   if( tracks.size() > nTracksOriginally ){
      // 0.0 is min scroll position, 1.0 is max scroll position.
      trackPanel.VerticalScroll( 1.0 );
   }
   else {
      auto pTrack = *tracks.Selected().begin();
      if (!pTrack)
         pTrack = *tracks.Any().begin();
      if (pTrack) {
         TrackFocus::Get(project).Set(pTrack);
         pTrack->EnsureVisible();
      }
   }

   return true;
}

///////////////////////////////////////////////////////////////////////////////
BEGIN_EVENT_TABLE(EffectDialog, wxDialogWrapper)
   EVT_BUTTON(wxID_OK, EffectDialog::OnOk)
END_EVENT_TABLE()

EffectDialog::EffectDialog(wxWindow * parent,
                           const TranslatableString & title,
                           int type,
                           int flags,
                           int additionalButtons)
: wxDialogWrapper(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, flags)
{
   mType = type;
   mAdditionalButtons = additionalButtons;
}

void EffectDialog::Init()
{
   long buttons = eOkButton;
   if ((mType != EffectTypeAnalyze) && (mType != EffectTypeTool))
   {
      buttons |= eCancelButton;
      if (mType == EffectTypeProcess)
      {
         buttons |= ePreviewButton;
      }
   }

   ShuttleGui S(this, eIsCreating);

   S.SetBorder(5);
   S.StartVerticalLay(true);
   {
      PopulateOrExchange(S);
      S.AddStandardButtons(buttons|mAdditionalButtons);
   }
   S.EndVerticalLay();

   Layout();
   Fit();
   SetMinSize(GetSize());
   Center();
}

/// This is a virtual function which will be overridden to
/// provide the actual parameters that we want for each
/// kind of dialog.
void EffectDialog::PopulateOrExchange(ShuttleGui & WXUNUSED(S))
{
   return;
}

bool EffectDialog::TransferDataToWindow()
{
   ShuttleGui S(this, eIsSettingToDialog);
   PopulateOrExchange(S);

   return true;
}

bool EffectDialog::TransferDataFromWindow()
{
   ShuttleGui S(this, eIsGettingFromDialog);
   PopulateOrExchange(S);

   return true;
}

bool EffectDialog::Validate()
{
   return true;
}

void EffectDialog::OnPreview(wxCommandEvent & WXUNUSED(evt))
{
   return;
}

void EffectDialog::OnOk(wxCommandEvent & WXUNUSED(evt))
{
   // On wxGTK (wx2.8.12), the default action is still executed even if
   // the button is disabled.  This appears to affect all wxDialogs, not
   // just our Effects dialogs.  So, this is a only temporary workaround
   // for legacy effects that disable the OK button.  Hopefully this has
   // been corrected in wx3.
   if (FindWindow(wxID_OK)->IsEnabled() && Validate() && TransferDataFromWindow())
   {
      EndModal(wxID_OK);
   }

   return;
}

//! Inject a factory for realtime effect states
#include "RealtimeEffectState.h"
static
RealtimeEffectState::EffectFactory::Scope scope{ &EffectManager::NewEffect };

/* The following registration objects need a home at a higher level to avoid
 dependency either way between WaveTrack or RealtimeEffectList, which need to
 be in different libraries that do not depend either on the other.

 WaveTrack, like AudacityProject, has a registry for attachment of serializable
 data.  RealtimeEffectList exposes an interface for serialization.  This is
 where we connect them.
 */
#include "RealtimeEffectList.h"
static ProjectFileIORegistry::ObjectReaderEntry projectAccessor {
   RealtimeEffectList::XMLTag(),
   [](AudacityProject &project) { return &RealtimeEffectList::Get(project); }
};

static ProjectFileIORegistry::ObjectWriterEntry projectWriter {
[](const AudacityProject &project, XMLWriter &xmlFile){
   RealtimeEffectList::Get(project).WriteXML(xmlFile);
} };

static WaveTrackIORegistry::ObjectReaderEntry waveTrackAccessor {
   RealtimeEffectList::XMLTag(),
   [](WaveTrack &track) { return &RealtimeEffectList::Get(track); }
};

static WaveTrackIORegistry::ObjectWriterEntry waveTrackWriter {
[](const WaveTrack &track, auto &xmlFile) {
   RealtimeEffectList::Get(track).WriteXML(xmlFile);
} };
