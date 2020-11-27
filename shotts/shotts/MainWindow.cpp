#include "pch.h"
#include "MainWindow.h"
#include "MainWindow.g.cpp"
#include "ShotTaker.h"
#include <microsoft.ui.xaml.window.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::shotts::implementation
{
   MainWindow::MainWindow()
   {
      InitializeComponent();
      Title(L"shotts");
   }
}

HWND shotts::implementation::MainWindow::GetWindowHandle() {
   HWND hwnd;
   Window window = this->try_as<Window>();
   window.as<IWindowNative>()->get_WindowHandle(&hwnd);
   return hwnd;
}

Windows::Foundation::IAsyncAction shotts::implementation::MainWindow::SaveShots(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const& e)
{
   using namespace Windows::Storage;
   Pickers::FolderPicker picker;
   picker.as<IInitializeWithWindow>()->Initialize(GetWindowHandle());
   picker.SuggestedStartLocation(Pickers::PickerLocationId::PicturesLibrary);
   StorageFolder folder = co_await picker.PickSingleFolderAsync();
   if (!folder) co_return;


}

void shotts::implementation::MainWindow::ImageClick(Windows::Foundation::IInspectable const& sender, Input::TappedRoutedEventArgs const& e)
{
   MainShot().Source(sender.as<Controls::Image>().Source());
}


Windows::Foundation::IAsyncAction shotts::implementation::MainWindow::OpenVideo(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const& e)
{
   using namespace Windows::Storage;
   Pickers::FileOpenPicker picker;

   picker.as<IInitializeWithWindow>()->Initialize(GetForegroundWindow());
   picker.FileTypeFilter().Append(L".mkv");
   picker.SuggestedStartLocation(Pickers::PickerLocationId::VideosLibrary);
   StorageFile file = co_await picker.PickSingleFileAsync();
   if (!file) co_return;

   std::optional<std::string> error;
   try {
      auto n_shots = static_cast<int>(ShotSlider().Value());

      //TakeShots can't be called on the UI thread, so call it in a background thread
      winrt::apartment_context ui_thread;
      co_await winrt::resume_background();

      shot_taker_.emplace(to_string(file.Path()));
      auto files = shot_taker_->TakeShots(n_shots);

      std::vector<Windows::Foundation::Uri> uris;
      uris.reserve(size(files));
      std::transform(begin(files), end(files), std::back_inserter(uris),
         [](auto&& file) { return Windows::Foundation::Uri(file.Path()); });

      //Resume on the UI thread for displaying shots
      co_await ui_thread;

      Microsoft::UI::Xaml::Media::Imaging::BitmapImage main_source(uris[0]);
      MainShot().Source(main_source);

      ShotBar().Children().Clear();

      int i = 0;
      for (auto&& uri : uris) {
         using namespace Controls;
         ColumnDefinition column;
         ShotBar().ColumnDefinitions().Append(column);

         Image img;
         Microsoft::UI::Xaml::Media::Imaging::BitmapImage source(uri);
         img.Source(source);
         img.Tapped(Input::TappedEventHandler(this, &MainWindow::ImageClick));
         //Hack: this should be based on the ScrollViewer size
         img.Height(150);
         Grid::SetColumn(img, i);
         img.Name(hstring(L"shot") + to_hstring(i));
         ShotBar().Children().Append(img);

         Button refresh;
         refresh.HorizontalAlignment(HorizontalAlignment::Right);
         refresh.VerticalAlignment(VerticalAlignment::Top);
         refresh.Background(Media::SolidColorBrush(Windows::UI::Colors::WhiteSmoke()));
         SymbolIcon icon(Symbol::Refresh);
         refresh.Content(icon);
         refresh.Name(hstring(L"icon") + to_hstring(i));
         Grid::SetColumn(refresh, i);

         ShotBar().Children().Append(refresh);
         ++i;
      }
   }
   catch (std::runtime_error& e) {
      error = e.what();
   }

   if (error) {
      Controls::ContentDialog errorDialog;
      errorDialog.Title(box_value(L"Error"));
      errorDialog.Content(box_value(to_hstring(*error)));
      errorDialog.CloseButtonText(L"Ok");
      errorDialog.XamlRoot(this->ShotSlider().XamlRoot());
      co_await errorDialog.ShowAsync();
   }
}


Windows::Foundation::IAsyncAction winrt::shotts::implementation::MainWindow::RefreshImage(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
{
   std::optional<std::string> error;
   try {
      //TakeShots can't be called on the UI thread, so call it in a background thread
      winrt::apartment_context ui_thread;
      co_await winrt::resume_background();

      auto files = shot_taker_->TakeShots(1);
      auto uri = Windows::Foundation::Uri(files[0].Path());

      //Resume on the UI thread for displaying shots
      co_await ui_thread;

      Microsoft::UI::Xaml::Media::Imaging::BitmapImage main_source(uri);
      MainShot().Source(main_source);

   }
   catch (std::runtime_error& e) {
      error = e.what();
   }

   if (error) {
      Controls::ContentDialog errorDialog;
      errorDialog.Title(box_value(L"Error"));
      errorDialog.Content(box_value(to_hstring(*error)));
      errorDialog.CloseButtonText(L"Ok");
      errorDialog.XamlRoot(this->ShotSlider().XamlRoot());
      co_await errorDialog.ShowAsync();
   }
}
