<?php

/**
 * @file
 * Hooks invoked by the Project Source API.
 */


/**
 * Implements hook_project_src_info().
 */
function project_src_github_project_src_info() {
  // This can be an expensive operation, so we cache our results.
  $cid = 'project_src_github_projects';
  if (!$projects = cache_get($cid)) {
    $github_client = github_api_client();
    $org = variable_get('project_src_github_org', '');
    try {
      $response = $github_client->api('organization')->repositories($org);
    }
    catch (Exception $e) {
      watchdog('project src github', 'An error occurred when attempting to load repositories for %org from GitHub. Message returned:<br /><br />@msg', array(
        '%org' => $org,
        '@msg' => $e->getMessage(),
      ), WATCHDOG_ERROR);
      $response = array();
    }

    // Build out project definitions as expected by this hook's invoker.
    foreach ($response as $project_info) {
      // Initialize this project.
      $project = array();
      $project['short_name'] = check_plain($project_info['name']);

      // Project-wide variables.
      $project['title'] = check_plain($project_info['name']);
      $project['creator'] = check_plain($project_info['owner']['login']);
      $project['project_status'] = 'published';
      $project['link'] = check_url($project_info['html_url']);
      if (preg_match_all("/^(\\d+).x-(\\d+).x$/is", $project_info['default_branch'], $version)) {
        $project['default_major'] = check_plain($version[2][0]);
      }

      // Variables required for GitHub API handling.
      $project['api'] = array(
        'id' => $project_info['id'],
        'path_with_namespace' => $project_info['full_name'],
        'branches' => array(),
      );

      // Get all branches on this project.
      try {
        $branches = $github_client->api('repo')->branches($project['creator'], $project['short_name']);
      }
      catch (Exception $e) {
        watchdog('project src github', 'An error occurred when attempting to load branches for %project (by %creator) from GitHub. Message returned:<br /><br />@msg', array(
          '%project' => $project['creator'],
          '%creator' => $project['short_name'],
          '@msg' => $e->getMessage(),
        ), WATCHDOG_ERROR);
        $branches = array();
      }
      foreach ($branches as $branch) {
        if (preg_match_all("/^(\\d+).x-(\\d+).x$/is", $branch['name'], $versions)) {
          $api_version = check_plain($versions[1][0] . '.x');
          $project['api_version'] = $api_version;
          $project['api']['branches'][$api_version][$branch['name']] = $branch;
          // @todo This isn't actually working...
          $project['supported_majors'] = implode(',', $versions[2]);
          $project['recommended_major'] = max($versions[2]);

          // Save off this project to the proper place in return array.
          $projects[$api_version][$project['short_name']] = $project;
        }
      }
    }

    // If projects were returned, cache them. Depending on the backend used to
    // generate the project, this process could be expensive.
    if (!empty($projects)) {
      cache_set($cid, $projects, 'cache');
    }
  }
  else {
    $projects = $projects->data;
  }

  return $projects;
}


/**
 * Implements hook_project_src_releases().
 */
function project_src_github_project_src_releases($short_name, $api_version, $info) {
  // Load all of our declared projects for this API version.
  $projects = project_src_get_projects($api_version, 'project_src_github');

  // Loop through all relevant branches from GitHub and create dev releases,
  // regardless of whether or not there are any tags for a release.
  $dev_cid = 'project_src_github_releases_' . $short_name . '_' . $api_version . '_dev';
  if (!$dev_releases = cache_get($dev_cid)) {
    $dev_releases = array();
    foreach ($info['api']['branches'][$api_version] as $branch) {
      $original = $branch['name'];
      $branch['name'] = $original . '-dev';
      $tarball_root = 'drupal/github-release-files/';
      $tarball_path = $info['api']['path_with_namespace'] . '/' . $original;
      $branch['tarball_url'] = url($tarball_root . $tarball_path . '/download.tar.gz', array(
        'absolute' => TRUE,
      ));
      $dev_releases[$original] = _project_src_github_format_release_info($branch, $info);
      $dev_releases[$original]['download_link'] = str_replace($branch['name'], $original, $dev_releases[$original]['download_link']);
    }
    // Cache these using CACHE_TEMPORARY to ensure dev releases stay up-to-date.
    cache_set($dev_cid, $dev_releases, 'cache', CACHE_TEMPORARY);
  }
  else {
    $dev_releases = $dev_releases->data;
  }

  // React if the incoming project is one of ours.
  if (isset($projects[$short_name])) {
    // Get this project.
    $cid = 'project_src_github_releases_' . $short_name . '_' . $api_version;
    if (!$tags = cache_get($cid)) {
      // Attempt to load all tags for the given release and API version.
      $github_client = github_api_client();
      $org = variable_get('project_src_github_org', '');
      try {
        $tags = $github_client->api('repo')->tags($org, $short_name);
      }
      catch (Exception $e) {
        watchdog('project src github', 'An error occurred when attempting to load tags for %project (by %creator) from GitHub. Message returned:<br /><br />@msg', array(
          '%project' => $short_name,
          '%creator' => $org,
          '@msg' => $e->getMessage(),
        ), WATCHDOG_ERROR);
        $tags = array();
      }

      $releases = array();
      // Loop through all release tags from GitLab and create releases.
      foreach ($tags as $tag) {

        // Match on tags of the proper form.
        if (preg_match_all("/^(\\d+).x-(\\d+).\\d+/is", $tag['name'], $version)) {
          $api = $version[1][0] . '.x';
          // Only return releases for the current API version.
          if ($api == $api_version) {
            // Custom URL for download.
            $tarball_root = 'drupal/github-release-files/';
            $tarball_path = $info['api']['path_with_namespace'] . '/' . $tag['name'];
            $tag['tarball_url'] = url($tarball_root . $tarball_path . '/download.tar.gz', array(
              'absolute' => TRUE,
            ));

            // Add the release to the master releases array.
            $releases[$tag['name']] = _project_src_github_format_release_info($tag, $info);
          }
        }
      }

      cache_set($cid, $releases, 'cache');
    }
    else {
      $releases = $tags->data;
    }
  }

  return $releases + $dev_releases;
}