<?php

/**
 * @file
 * Mime Mail implementations of MailSystemInterface.
 */

/**
 * Modify the Drupal mail system to send HTML emails.
 *
 * The correct name for this class should be: MimeMailEnhancerSystem
 * However webform has an ?incomplete? implementation for picking up MailSystemInterface
 * implementations sites/all/modules/contrib/webform/webform.module:3828
 * (hardcoded) and fortunately strpos check can be hacked by ending the class
 * name as: MimeMailSystem -> hence: EnhancedMimeMailSystem
 */
class EnhancedMimeMailSystem implements MailSystemInterface {

  /**
   * Concatenate and wrap the e-mail body for HTML mails.
   *
   * @param array $message
   *   A message array, as described in hook_mail_alter() with optional
   *   parameters described in mimemail_prepare_message().
   *
   *   The $message has alerady passed through all hook_mail_alters
   *
   * @return array
   *   The formatted $message.
   */
  public function format(array $message) {
    $this->prepareMessage($message);
    return $message;
  }

  /**
   * @param array $message
   * @return array
   */
  protected function prepareMessage(&$message) {
    module_load_include('inc', 'mimemail');

    //@todo - add custom 'pre_process' hook here

    $module = $message['module'];
    $key = $message['key'];
    $to = $message['to']; //??@what if (like webform) we have comma separated list of to addresses?
    $from = $message['from'];
    $subject = $message['subject'];

    $headers = isset($message['params']['headers']) ? $message['params']['headers'] : array();
    $plain = isset($message['params']['plain']) ? $message['params']['plain'] : NULL;
    $plaintext = isset($message['params']['plaintext']) ? $message['params']['plaintext'] : NULL;
    $attachments = isset($message['params']['attachments']) ? $message['params']['attachments'] : array();

    $site_name = variable_get('site_name', 'Drupal');
    $site_mail = variable_get('site_mail', ini_get('sendmail_from'));
    $simple_address = variable_get('mimemail_simple_address', 0);

    if(!isset($message["params"]["mail_theme_stylesheets"]) ||
       !is_array($message["params"]["mail_theme_stylesheets"]) ||
       !count($message["params"]["mail_theme_stylesheets"])) {
      $message["params"]["mail_theme_stylesheets"] = ["mimemail_enhancer"];
    }


    if (is_array($message['body'])) {
      $message['body'] = implode("\n\n", $message['body']);
    }

    // Override site mails default sender when using default engine.
    if ((empty($from) || $from == $site_mail)
        && variable_get('mimemail_engine', 'mimemail') == 'mimemail') {
      $mimemail_name = variable_get('mimemail_name', $site_name);
      $mimemail_mail = variable_get('mimemail_mail', $site_mail);
      $from = array(
        'name' => !empty($mimemail_name) ? $mimemail_name : $site_name,
        'mail' => !empty($mimemail_mail) ? $mimemail_mail : $site_mail,
      );
    }

    //checking if plain or html
    if (is_object($to) && isset($to->data['mimemail_textonly'])) {
      $plain = $to->data['mimemail_textonly'];
    } else if (is_string($to) && valid_email_address($to)) {
      if (is_object($account = user_load_by_mail($to)) && isset($account->data['mimemail_textonly'])) {
        $plain = $account->data['mimemail_textonly'];
        $to = $account;
      }
    }
    if (is_null($plain)){
      $plain = false;
    }

    // Removing newline character introduced by _drupal_wrap_mail_line();
    $message['subject'] = str_replace(array("\n"), '', trim(drupal_html_to_text($subject)));

    //THEME
    $theme_vars = [
      'module' => $message["module"],
      'key' => $message["key"],
      'plain' => $plain,
      'message' => &$message,
    ];
    $hook = [
      'mimemail_enhancer_message__' . $module .'__'. $key,
    ];
    $message['themed_body'] = theme($hook, $theme_vars);


    //do stuff with message
    $this->replaceTokens($message);
    $this->checkAttachments($message);
    $this->loadStylesheets($message);
    $this->applyStylesheets($message);

    //@todo - add custom 'post_process' hook here



    //finalize message for sending
    $this->attachInlineImages($message);
    $plain = $plain || variable_get('mimemail_textonly', 0);
    $from = mimemail_address($from);
    $mail = mimemail_html_body($message['themed_body_cids'], $subject, $plain, $plaintext, $attachments);
    $headers = array_merge($message['headers'], $headers, $mail['headers']);
    //
    $message['from'] = $from;
    $message['to'] = mimemail_address($to, $simple_address);
    $message['body'] = $mail['body'];
    $message['headers'] = mimemail_headers($headers, $from);
  }

  /**
   * @param array $message
   */
  protected function attachInlineImages(&$message) {
    //add files in cssContent as inline attachments and replace reference with cid
    //using (mimemail/mimemail.inc:140) _mimemail_replace_files callback
    $pattern = '/(url\([\'"]?)([^\')"]+)([\'"]?\))/mis';
    $message['themed_body_cids'] = preg_replace_callback($pattern, '_mimemail_replace_files', $message['themed_body']);
  }

  /**
   * @param array $message
   */
  protected function applyStylesheets(&$message) {
    if(isset($message['params']['mail_theme_css'])) {
      $mail_css = $message['params']['mail_theme_css'];
      $mail_css = preg_replace('|\{|', "\n{\n", $mail_css);
      $mail_css = preg_replace('|\}|', "\n}\n", $mail_css);
      $compressor = new mimemail_compress($message['themed_body'], $mail_css);
      $message['themed_body'] = $compressor->compress();
    }
  }


  /**
   * @param array $message
   */
  protected function loadStylesheets(&$message) {
    if(!isset($message["params"]["mail_theme_stylesheets"]) ||
       !is_array($message["params"]["mail_theme_stylesheets"]) ||
       !count($message["params"]["mail_theme_stylesheets"])) {
      return;
    }

    $modulePath = drupal_get_path('module', 'mimemail_enhancer');
    $message['params']['mail_theme'] = mailsystem_get_mail_theme();
    $message['params']['mail_theme_path'] = drupal_get_path('theme', $message['params']['mail_theme']);


    //collect css files
    $expr = '#^('.implode('|', $message["params"]["mail_theme_stylesheets"]).')\.css$#';
    $mail_stylesheets["css"] = file_scan_directory($message['params']['mail_theme_path'], $expr);
    if(!count($mail_stylesheets["css"])) {
      $mail_stylesheets["css"] = file_scan_directory($modulePath, $expr);
    }

    //collect less files if we have support
    if(!count($mail_stylesheets["css"])) {
      if (module_exists("less")) {
        $expr = '#^('.implode('|', $message["params"]["mail_theme_stylesheets"]).')\.less$#';
        $mail_stylesheets["less"] = file_scan_directory($message['params']['mail_theme_path'], $expr);
        if(!count($mail_stylesheets["less"])) {
          $mail_stylesheets["less"] = file_scan_directory($modulePath, $expr);
        }
        //preprocess less files
        if(count($mail_stylesheets["less"])) {
          $styles = [
            '#items' => [],
          ];
          /** @var object $lessFile */
          foreach($mail_stylesheets["less"] as $lessKey => $lessFile) {
            $styles['#items'][$lessKey] = [
              'data' => $lessFile->uri,
            ];
          }
          $styles = _less_pre_render($styles);
          //
          foreach($styles['#items'] as $prepocessedLess) {
            ///$mail_stylesheets["css"][] = $prepocessedLess["less"]["output_file"];
            $cssFile = str_replace(DRUPAL_ROOT . '/', '', drupal_realpath($prepocessedLess["less"]["output_file"]));
            $info = pathinfo($cssFile);
            $file = new \stdClass();
            $file->uri = $cssFile;
            $file->filename = $info["basename"];
            $file->name = str_replace('.'.$info["extension"], '', $file->filename);
            $mail_stylesheets["css"][$cssFile] = $file;
          }
        }
      }
    }

    $mail_css = '';
    if(count($mail_stylesheets["css"])) {
      /** @var \stdClass $mail_stylesheet */
      foreach($mail_stylesheets["css"] as $mail_stylesheet) {
        $css = drupal_load_stylesheet($mail_stylesheet->uri, TRUE);
        $css = $this->correctResourcePaths($css, dirname($mail_stylesheet->uri));
        $mail_css .= $css;
      }
    }

    $message['params']['mail_theme_stylesheet_files'] = $mail_stylesheets;
    $message['params']['mail_theme_css'] = $mail_css;
  }

  /**
   * @param string $str - the string to make substitutions on
   * @param string $pathReference - this must be relative to DRUPAL_ROOT
   * @return string
   */
  protected function correctResourcePaths($str, $pathReference) {
    //match url(...) strings in css
    preg_match_all('#url\(["\']?(?<respath>[^"\')]+)["\']?\)#', $str, $paths);
    if(isset($paths["respath"]) && count($paths["respath"])) {
      //make resource paths become the key
      $resourcePaths = array_flip($paths["respath"]);
      //set the correct path as the value
      foreach($resourcePaths as $respath => &$fullpath) {
        $realpath = realpath(DRUPAL_ROOT . '/' . $pathReference . '/' . $respath);
        $fullpath = $realpath ? str_replace(DRUPAL_ROOT, '', $realpath) : $respath;
      }
      $str = str_replace(array_keys($resourcePaths), array_values($resourcePaths), $str);
    }
    return $str;
  }

  /**
   * @param array $message
   */
  protected function replaceTokens(&$message) {
    $options = ['clear' => FALSE];
    $context = $message["params"];

    //replace tokens
    $message['to'] = token_replace($message['to'], $context, $options);
    $message['subject'] = token_replace($message['subject'], $context, $options);
    $message['themed_body'] = token_replace($message['themed_body'], $context, $options);
  }

  /**
   * @param array $message
   */
  protected function checkAttachments(&$message) {
    //attachments -  @todo: check this
    /*
    // Prepare the array of the attachments.
    $attachments = array();
    if(isset($message["params"]['attachments'])) {
      $attachments_string = trim($message["params"]['attachments']);
      if (!empty($attachments_string)) {
        $attachment_lines = array_filter(explode("\n", trim($attachments_string)));
        foreach ($attachment_lines as $filepath) {
          $attachments[] = array(
            'filepath' => trim($filepath),
          );
        }
      }
    }
    $message['params']['attachments'] = $attachments;
    */
  }

  /**
   * Send the message
   *
   * @todo: for now we are using mimemail engine send - do we need our own?
   *
   * @param array $message
   *   A message array, as described in hook_mail_alter() with optional
   *   parameters described in mimemail_prepare_message().
   *
   * @return boolean
   *   TRUE if the mail was successfully accepted, otherwise FALSE.
   */
  public function mail(array $message) {
    if(variable_get('mimemail_enhancer_theme_debug', false)) {
      $debuggers = variable_get('mimemail_enhancer_debuggers', []);
      /** object $user */
      global $user;
      if(in_array($user->uid, $debuggers)) {
        $this->dumpDebugInfo($message);
        die();
      }
    }
    //send the messgae
    $engine = variable_get('mimemail_engine', 'mimemail');
    $mailengine = $engine . '_mailengine';
    if (!$engine || !function_exists($mailengine)) {
      return FALSE;
    }
    return $mailengine('send', $message);
  }

  /**
   * @param array $message
   */
  protected function dumpDebugInfo($message) {
    require(drupal_get_path('module', 'mimemail_enhancer') . '/debug/debug.php');
  }

}